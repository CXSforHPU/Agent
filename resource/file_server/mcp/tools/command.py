"""按命令行拉起一个 stdio MCP 服务器，把它的工具挂进网关。

Windows-MCP 这类服务器在 PC 上是本地进程，跟网关之间走 stdio 传输。网关把
它当子进程拉起来，握手后把工具转发给设备端，设备端只看到一个 HTTP 端点。

暴露出去的工具名只保留字母、数字、下划线和连字符，并统一转小写。设备端的
函数名只允许这些字符，改过之后名字才能原样传回来，路由才对得上。
"""
import os

from .remote import RemoteTool
from .stdio import StdioClient, StdioError

PROTOCOL_VERSION = "2025-06-18"


def build_tools(name, command, args=None, env=None, cwd=None, timeout=60.0):
    """拉起进程并握手，把远端的工具变成一串 Tool。

    握手失败返回空列表，远端进程会被停掉。
    """
    environment = dict(os.environ)
    if env:
        environment.update(env)
    client = StdioClient(command, args, env=environment, cwd=cwd, timeout=timeout)
    try:
        client.start()
        client.request("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "rt-thread-agent-gateway", "version": "1.0.0"},
        })
        client.notify("notifications/initialized")
        result = client.request("tools/list")
    except StdioError as exc:
        print("[mcp] 服务器 {} 握手失败：{}".format(name, exc))
        client.stop()
        return []

    tools = []
    used = set()
    for item in result.get("tools") or []:
        original = item.get("name")
        if not original:
            continue
        exposed = _exposed_name(original, used)
        used.add(exposed)
        tools.append(RemoteTool(
            client,
            original,
            exposed,
            item.get("description", ""),
            item.get("inputSchema") or {"type": "object", "properties": {}},
            namespace=name + "__",
            prefix=name + "__",
        ))
    print("[mcp] 已接入服务器 {}，工具 {} 个".format(name, len(tools)))
    return tools


def _exposed_name(original, used):
    """远端工具名转成网关暴露的名字。"""
    safe = "".join(c if (c.isascii() and (c.isalnum() or c in "_-")) else "_"
                   for c in original).lower()
    if not safe:
        safe = "tool"
    name = safe
    index = 2
    while name in used:
        name = "{}_{}".format(safe, index)
        index += 1
    return name
