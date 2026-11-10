"""网关的分层实现。

按层拆开，依赖只向里指：

    transport   HTTP 收发            transport.py
    service     用例调度            service.py
    registry    工具表与命名        registry.py
    shaping     回包裁剪            shaping.py
    runtime     后台事件循环        runtime.py
    protocol    协议常量与报文      protocol.py

外层可以叫里层，里层不反过来叫外层。工具在 tools/ 目录里，不 import 这一层。
"""
from .protocol import PROTOCOL_VERSION, SERVER_INFO, error, result
from .registry import ToolRegistry
from .runtime import EventLoop
from .service import McpService
from .shaping import ResponseShaper
from .transport import McpHandler, create_server

__all__ = ["PROTOCOL_VERSION", "SERVER_INFO", "result", "error", "ToolRegistry",
           "EventLoop", "McpService", "ResponseShaper", "McpHandler",
           "create_server"]
