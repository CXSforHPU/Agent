"""MCP 的 stdio 传输客户端。

MCP 的 stdio 传输用换行分隔 JSON-RPC 报文，一行一条，不能带内嵌换行。
这个客户端给 ODR 里的连接器用：Windows 上的 MCP 服务器是本地进程，主机端
按清单里的 command/args 拉起来，再用 stdio 通信。
"""
import json
import subprocess
import threading


class StdioError(Exception):
    """握手失败、进程退出或超时。"""


class StdioClient:
    """一个 MCP 服务器子进程的 stdio 客户端。"""

    def __init__(self, command, args=None, env=None, cwd=None, timeout=10.0):
        self.command = command
        self.args = list(args or [])
        self.timeout = timeout
        self._process = None
        self._next_id = 0
        self._pending = {}
        self._lock = threading.Lock()
        self._reader = None
        self._env = env
        self._cwd = cwd
        self._closed = False

    def start(self):
        """拉起子进程并启动读线程。停过之后不再拉起。"""
        with self._lock:
            if self._process or self._closed:
                return
            self._process = subprocess.Popen(
                [self.command] + self.args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=self._env,
                cwd=self._cwd,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        stream = self._process.stdout
        try:
            for line in stream:
                line = line.strip()
                if not line:
                    continue
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                self._dispatch(message)
        except (ValueError, OSError):
            pass
        finally:
            self._fail_pending("server closed the connection")

    def _dispatch(self, message):
        if not isinstance(message, dict):
            return
        request_id = message.get("id")
        if request_id is None:
            return
        with self._lock:
            event = self._pending.pop(request_id, None)
        if event:
            event[1] = message
            event[0].set()

    def _fail_pending(self, reason):
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for event in pending:
            event[1] = {"error": {"code": -32603, "message": reason}}
            event[0].set()

    def _send(self, message):
        if not self._process or self._process.poll() is not None:
            raise StdioError("server process is not running")
        payload = json.dumps(message, ensure_ascii=False)
        if "\n" in payload:
            raise StdioError("message must not contain a newline")
        try:
            self._process.stdin.write(payload + "\n")
            self._process.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise StdioError("write to the server failed: {}".format(exc))

    def request(self, method, params=None):
        """发一条请求并等回包。"""
        self.start()
        with self._lock:
            self._next_id += 1
            request_id = self._next_id
            event = [threading.Event(), None]
            self._pending[request_id] = event
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)
        if not event[0].wait(self.timeout):
            with self._lock:
                self._pending.pop(request_id, None)
            raise StdioError("timeout waiting for {}".format(method))
        reply = event[1]
        if "error" in reply:
            error = reply["error"] or {}
            raise StdioError("{} failed: {}".format(method, error.get("message", "unknown")))
        return reply.get("result", {})

    def notify(self, method, params=None):
        """发一条通知，不等回包。"""
        self.start()
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def stop(self):
        """结束子进程。停多次没影响。"""
        with self._lock:
            if self._closed:
                return
            self._closed = True
            process = self._process
            self._process = None
        if process is None:
            return
        try:
            process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            process.terminate()
            process.wait(timeout=5)
        except (subprocess.TimeoutExpired, OSError):
            process.kill()
