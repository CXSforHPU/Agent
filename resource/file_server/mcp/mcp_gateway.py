#!/usr/bin/env python3
"""RT-Thread Agent 的 MCP 网关。

这里是组装根：解析配置、建对象、装工具、起服务。实现按层拆在两个目录里：

    gateway/   网关本身：传输、用例、工具表、回包裁剪、事件循环、协议
    tools/     工具实现：基类、内置文件工具、联网搜索、用户自己的工具

依赖只向里指：外层可以叫里层，里层不反过来叫外层。工具不 import 网关。

设备端的 MCP 客户端只能走 HTTP（Streamable HTTP 传输）。Windows 上的 MCP
服务器分三类：ODR 注册的连接器、命令行拉起的服务器（比如 Windows-MCP），
传输方式都是 stdio，需要在 PC 上拉起进程；另一类是本机文件工具，直接读写
磁盘。本网关把这些工具统一成一个 HTTP 端点，设备端只需要配置这一个地址。

网关自己还带一个联网搜索工具，搜索服务不配也能注册，调用时回一句说明。

执行走异步：网关起一个事件循环，工具的 execute 是协程，阻塞的活儿丢到线程里
跑，一个工具在跑不会卡住整个网关。

只用标准库，不需要 pip install。

用法：
    python mcp_gateway.py --host 0.0.0.0 --port 8765 --root C:\\Users\\me
    python mcp_gateway.py --windows-mcp
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from config import parse_args
from gateway import McpService, ResponseShaper, ToolRegistry, create_server
from gateway.runtime import EventLoop
from tools import USER_TOOL_DIR, ToolContext, load_user_tools
from tools.media import FileServer


def _add_user_tools(registry, settings):
    """加载用户自己的工具目录。

    内置的 tools/user_tools 目录始终加载，--user-tools 指定的目录额外加载。
    目录里的 .py 文件按路径加载，文件里继承 Tool 的类自动实例化并注册。
    """
    directories = []
    if not settings.get("no_user_tools"):
        directories.append(USER_TOOL_DIR)
    for item in (settings.get("user_tools") or []):
        directories.append(item if os.path.isabs(item) else os.path.abspath(item))

    tools = []
    for directory in directories:
        found = load_user_tools(directory)
        if not found:
            print("[mcp] 用户工具目录里没有工具：{}".format(directory))
        tools.extend(found)
    if not tools:
        print("[mcp] 没有加载用户工具")
        return
    accepted = registry.add_tools(tools)
    print("[mcp] 用户工具 {} 个：{}".format(
        len(accepted), ", ".join(tool.exposed_name for tool in accepted)))
    if any(tool.name == "web_search" for tool in accepted):
        print("[mcp] web_search 已接入：{}（对接方式 {}）".format(
            settings.get("web_search.url"), settings.get("web_search.mode")))


def _add_windows_mcp(registry, settings):
    """接入 Windows-MCP。命令按 uvx windows-mcp serve 之类的形式给出。"""
    command = list(settings.get("windows_mcp_cmd") or [])
    if not command:
        print("[mcp] 没给 Windows-MCP 的启动命令，跳过")
        return
    found = shutil.which(command[0])
    if not found:
        print("[mcp] 找不到 {}，先装 uv（https://astral.sh/uv）再试".format(command[0]))
        return
    try:
        from tools.command import build_tools
        tools = build_tools("win", found, command[1:],
                            timeout=settings.get("server_timeout"))
        registry.add_tools(tools)
    except Exception as exc:
        print("[mcp] Windows-MCP 不可用（{}）".format(exc))


def _add_odr(registry, settings):
    """接入 Windows ODR 注册表里的服务器。"""
    try:
        from tools.odr import build_tools as build_odr_tools
        tools = build_odr_tools(timeout=settings.get("server_timeout"))
        registry.add_tools(tools)
        if not tools:
            print("[mcp] ODR 里没有可用服务器，只提供内置文件工具")
    except Exception as exc:
        print("[mcp] ODR 不可用（{}），只用内置文件工具".format(exc))


def main():
    settings = parse_args()

    file_server = None
    if not settings.get("no_file_server"):
        file_server = FileServer(settings.get("file_server"),
                                 settings.get("file_server_url"))

    context = ToolContext(file_server=file_server, settings=settings)
    registry = ToolRegistry(context)
    shaper = ResponseShaper(max_text_bytes=settings.get("max_text_bytes"),
                            context=context)
    runtime = EventLoop()
    service = McpService(registry, shaper, runtime, context=context,
                         call_timeout=settings.get("call_timeout"))

    from tools.file import build_tools as build_file_tools
    registry.add_tools(build_file_tools(settings.get("root")))

    _add_user_tools(registry, settings)

    if settings.get("windows_mcp"):
        _add_windows_mcp(registry, settings)

    if not settings.get("no_odr"):
        _add_odr(registry, settings)

    runtime.start()
    server = create_server(settings.get("host"), settings.get("port"), service)
    pid_file = settings.get("pid_file")
    if pid_file:
        with open(pid_file, "w", encoding="utf-8") as handle:
            handle.write(str(os.getpid()))
    print("[mcp] 网关已启动：http://{}:{}/mcp".format(
        settings.get("host"), settings.get("port")))
    print("[mcp] 设备端配置 PKG_AGENT_MCP_SERVER_URL=http://<本机IP>:{}/mcp".format(
        settings.get("port")))
    print("[mcp] 回包文本上限 {} 字节".format(settings.get("max_text_bytes")))
    print("[mcp] 工具 {} 个".format(len(registry.list_tools())))
    if file_server:
        print("[mcp] 图片存到文件服务器：{}".format(file_server.upload_url))
    else:
        print("[mcp] 没接文件服务器，图片不落盘")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[mcp] 收到中断，退出")
    finally:
        server.server_close()
        registry.close()
        runtime.stop()
        if pid_file:
            try:
                os.remove(pid_file)
            except OSError:
                pass


if __name__ == "__main__":
    main()
