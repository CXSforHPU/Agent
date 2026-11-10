"""MCP 的协议常量与 JSON-RPC 报文。

这一层不认识工具，也不认识 HTTP，只管协议长什么样：版本号、服务端信息、
错误码，还有成功和失败两种回包的形状。
"""

PROTOCOL_VERSION = "2025-06-18"
SERVER_INFO = {"name": "rt-thread-agent-gateway", "version": "1.0.0"}

JSONRPC_PARSE_ERROR = -32700
JSONRPC_INVALID_REQUEST = -32600
JSONRPC_METHOD_NOT_FOUND = -32601
JSONRPC_INVALID_PARAMS = -32602


def result(request_id, payload):
    """一条成功的回包。"""
    return {"jsonrpc": "2.0", "id": request_id, "result": payload}


def error(request_id, code, message):
    """一条失败的回包。"""
    return {"jsonrpc": "2.0", "id": request_id,
            "error": {"code": code, "message": message}}
