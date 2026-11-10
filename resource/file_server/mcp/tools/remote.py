"""外部 MCP 服务器报上来的工具。

Windows-MCP、ODR 注册的连接器这些服务器有自己的工具清单，网关握手之后把
清单里的每一项变成一个 RemoteTool，设备端看到的就是普通的 MCP 工具。

工具名、说明和参数表都来自远端，只有调用这一步转发给远端。
"""
import asyncio

from .base import Tool


class RemoteTool(Tool):
    """远端服务器上的一个工具。"""

    def __init__(self, client, remote_name, name, description, parameters,
                 namespace=None, prefix=None):
        super().__init__(name=name, description=description, parameters=parameters,
                         namespace=namespace, prefix=prefix, closer=client.stop)
        self._client = client
        self._remote_name = remote_name

    async def execute(self, **kwargs):
        """把调用转发给远端，原样带回远端的回包。"""
        result = await asyncio.to_thread(self._client.request, "tools/call",
                                        {"name": self._remote_name,
                                         "arguments": kwargs})
        return {"content": result.get("content") or [],
                "isError": bool(result.get("isError"))}
