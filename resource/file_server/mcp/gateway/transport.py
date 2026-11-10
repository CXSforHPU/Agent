"""HTTP 传输。

设备端的 MCP 客户端只认 Streamable HTTP 传输。这一层把 HTTP 请求翻成
JSON-RPC 调用，再把回包写回去。它只认识报文格式和 HTTP，不认识工具。

会话 ID 在 initialize 的时候下发，之后的请求带上来就照着回。
"""
import json
import sys
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import protocol


class McpHandler(BaseHTTPRequestHandler):
    """把 HTTP 请求翻译成 JSON-RPC 调用。"""

    protocol_version = "HTTP/1.1"
    server_version = "AgentMcpGateway/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mcp] " + (fmt % args) + "\n")

    def _reply(self, status, payload, session_id=None):
        body = b"" if payload is None else json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if session_id:
            self.send_header("Mcp-Session-Id", session_id)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_POST(self):
        service = self.server.service
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            request = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self._reply(400, protocol.error(None, protocol.JSONRPC_PARSE_ERROR,
                                           "invalid json"))
            return

        response, status = service.dispatch(request)
        if status == 202:
            self._reply(202, None)
            return
        session_id = self.headers.get("Mcp-Session-Id")
        if isinstance(request, dict) and request.get("method") == "initialize":
            session_id = session_id or uuid.uuid4().hex
        self._reply(status, response, session_id)

    def do_GET(self):
        self._reply(405, {"error": "use POST for JSON-RPC"})

    def do_DELETE(self):
        self._reply(204, None)


def create_server(host, port, service):
    """建 HTTP 服务器，把服务层挂上去。"""
    server = ThreadingHTTPServer((host, port), McpHandler)
    server.daemon_threads = True
    server.service = service
    return server
