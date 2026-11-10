"""Windows ODR（设备代理注册表）接入。

ODR 是 Windows 11 上的 MCP 服务器注册表，`odr.exe list` 返回 JSON 数组，
每项的清单里有 server.mcp_config.command 和 server.mcp_config.args，
按这个清单用 stdio 拉起进程就是一个 MCP 服务器。

本机是 Windows 10，没有 odr.exe，build_tools() 返回空列表，网关只提供内置文件
工具。Windows 11 上同一个网关会自动把 ODR 里的服务器工具也挂进来。
"""
import json
import os
import shutil
import subprocess

from .remote import RemoteTool
from .stdio import StdioClient, StdioError

PROTOCOL_VERSION = "2025-06-18"


def _find_odr():
    """定位 odr.exe。先看 PATH，再看 Windows 目录。"""
    found = shutil.which("odr.exe")
    if found:
        return found
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    candidate = os.path.join(system_root, "System32", "odr.exe")
    return candidate if os.path.isfile(candidate) else None


def _server_id(entry, index):
    for key in ("id", "name", "displayName"):
        value = entry.get(key)
        if isinstance(value, str) and value:
            return value
    return "server{}".format(index)


def _mcp_config(entry):
    manifest = entry.get("manifest") or {}
    server = manifest.get("server") or {}
    return server.get("mcp_config") or {}


def build_tools(timeout=10.0):
    """列出并握手 ODR 里的服务器，把远端的工具变成一串 Tool。

    没有 odr.exe 或者没有可用服务器时返回空列表。
    """
    odr = _find_odr()
    if not odr:
        print("[mcp] 找不到 odr.exe，跳过 ODR 接入")
        return []
    try:
        output = subprocess.run([odr, "list"], capture_output=True, text=True,
                                timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        print("[mcp] odr.exe list 失败：{}".format(exc))
        return []
    if output.returncode != 0:
        print("[mcp] odr.exe list 返回 {}".format(output.returncode))
        return []
    try:
        entries = json.loads(output.stdout or "[]")
    except ValueError:
        print("[mcp] odr.exe list 的输出不是 JSON")
        return []
    if not isinstance(entries, list):
        return []

    tools = []
    for index, entry in enumerate(entries):
        tools.extend(_attach(entry, index, timeout))
    return tools


def _attach(entry, index, timeout):
    """握手一个 ODR 服务器，把它的工具变成 Tool 列表。"""
    config = _mcp_config(entry)
    command = config.get("command")
    args = config.get("args") or []
    if not command:
        print("[mcp] 服务器 {} 没有 mcp_config.command，跳过".format(_server_id(entry, index)))
        return []
    name = _server_id(entry, index)
    client = StdioClient(command, args, env=config.get("env"), timeout=timeout)
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
    for item in result.get("tools") or []:
        tool_name = item.get("name")
        if not tool_name:
            continue
        tools.append(RemoteTool(
            client,
            tool_name,
            tool_name,
            item.get("description", ""),
            item.get("inputSchema") or {"type": "object", "properties": {}},
            namespace=name + "__",
            prefix=name,
        ))
    print("[mcp] 已接入 ODR 服务器 {}，工具 {} 个".format(name, len(tools)))
    return tools
