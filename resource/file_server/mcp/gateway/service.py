"""MCP 的用例层。

这一层是网关的脑子：收到一条 JSON-RPC 请求，弄清楚要干什么，去工具表里把
工具找出来，跑它，再把结果裁好装回回包。

它不认识 HTTP，也不认识配置文件。HTTP 由 transport 层负责，工具在 tools/
目录里。
"""
import concurrent.futures

from tools.base import as_result
from tools.context import ToolContext, reset, use

from . import protocol


class McpService:
    """把 JSON-RPC 请求变成工具调用。"""

    def __init__(self, registry, shaper, runtime, context=None,
                 call_timeout=60.0):
        self._registry = registry
        self._shaper = shaper
        self._runtime = runtime
        self._context = context or ToolContext()
        self.call_timeout = call_timeout

    def dispatch(self, request):
        """处理一条 JSON-RPC 请求，返回 (响应对象或 None, HTTP 状态码)。

        这是给 HTTP 线程用的入口，协程丢到事件循环里跑。
        """
        try:
            return self._runtime.submit(self.handle(request), self.call_timeout + 5)
        except concurrent.futures.TimeoutError:
            return protocol.error(None, protocol.JSONRPC_METHOD_NOT_FOUND,
                                  "request timed out"), 200

    async def handle(self, request):
        """处理一条 JSON-RPC 请求，返回 (响应对象或 None, HTTP 状态码)。"""
        if not isinstance(request, dict):
            return protocol.error(None, protocol.JSONRPC_INVALID_REQUEST,
                                  "invalid request"), 200
        request_id = request.get("id")
        method = request.get("method")
        params = request.get("params") or {}
        if not isinstance(method, str):
            return protocol.error(request_id, protocol.JSONRPC_INVALID_REQUEST,
                                  "missing method"), 200

        if method == "initialize":
            result = {
                "protocolVersion": protocol.PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": protocol.SERVER_INFO,
            }
            return protocol.result(request_id, result), 200
        if method == "notifications/initialized":
            return None, 202
        if method == "ping":
            return protocol.result(request_id, {}), 200
        if method == "tools/list":
            return protocol.result(request_id,
                                   {"tools": self._registry.list_tools()}), 200
        if method == "tools/call":
            return await self._call_tool(request_id, params), 200
        return protocol.error(request_id, protocol.JSONRPC_METHOD_NOT_FOUND,
                              "method not found: {}".format(method)), 200

    async def _call_tool(self, request_id, params):
        """调一个工具，把结果装进回包。"""
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if not isinstance(name, str) or not name:
            return protocol.error(request_id, protocol.JSONRPC_INVALID_PARAMS,
                                  "missing tool name")
        if not isinstance(arguments, dict):
            return protocol.error(request_id, protocol.JSONRPC_INVALID_PARAMS,
                                  "arguments must be an object")
        tool = self._registry.find(name)
        if tool is None:
            return protocol.error(request_id, protocol.JSONRPC_INVALID_PARAMS,
                                  "unknown tool: {}".format(name))
        try:
            content, is_error = await self._run_tool(tool, name, arguments)
        except Exception as exc:  # 工具内部异常要回给模型，不能让连接挂掉
            return protocol.result(request_id, {
                "content": [{"type": "text", "text": "tool failed: {}".format(exc)}],
                "isError": True,
            })
        return protocol.result(request_id, {
            "content": await self._shaper.shape(content, name),
            "isError": bool(is_error),
        })

    async def _run_tool(self, tool, name, arguments):
        """执行一个工具，返回 (content 数组, 是否失败)。

        调用期间把上下文放进去，工具里存的图片和音频用工具名起名。
        """
        token = use(self._context.for_tool(name))
        try:
            raw = await tool.execute(**arguments)
        finally:
            reset(token)
        return as_result(raw)
