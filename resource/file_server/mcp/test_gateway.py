#!/usr/bin/env python3
"""MCP 网关的主机侧自测脚本。

按设备端的流程走一遍：initialize → notifications/initialized → tools/list →
tools/call，顺带检查会话 ID 是否按预期回传。只用标准库。

用法：
    python test_gateway.py --url http://127.0.0.1:8765/mcp
"""
import argparse
import asyncio
import base64
import contextlib
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request

PROTOCOL_VERSION = "2025-06-18"

# 板端的接收缓冲上限，板端装不下更大的回包
DEVICE_BODY_LIMIT = 32768

# 假文件服务器给的下载地址
FAKE_URL = "http://127.0.0.1:8000/download/00000000-0000-0000-0000-000000000000"


@contextlib.contextmanager
def env(**values):
    """临时改环境变量，值给 None 表示删掉这一项。"""
    saved = {key: os.environ.get(key) for key in values}
    for key, value in values.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value
    try:
        yield
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class Client:
    """最小 MCP HTTP 客户端。"""

    def __init__(self, url, timeout=10.0):
        self.url = url
        self.timeout = timeout
        self.session_id = None
        self.next_id = 0
        self.last_size = 0

    def post(self, payload):
        body = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        request = urllib.request.Request(self.url, data=body, headers=headers, method="POST")
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            raw = response.read()
            session = response.headers.get("Mcp-Session-Id")
            if session:
                self.session_id = session
            self.last_size = len(raw)
            return response.status, raw

    def request(self, method, params=None):
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        status, raw = self.post(payload)
        if status != 200:
            raise RuntimeError("{} 返回 HTTP {}".format(method, status))
        return json.loads(raw.decode("utf-8"))

    def notify(self, method, params=None):
        payload = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        status, _raw = self.post(payload)
        if status != 202:
            raise RuntimeError("{} 返回 HTTP {}，期望 202".format(method, status))


def check(condition, label, failures):
    print("[{}] {}".format("PASS" if condition else "FAIL", label))
    if not condition:
        failures.append(label)


class FakeFileServer:
    """假文件服务器，只看上传了什么，不真的存。"""

    def __init__(self, url=FAKE_URL):
        self.url = url
        self.last = None

    def upload(self, data, filename, mime):
        self.last = (data, filename, mime)
        return self.url


def make_shaper(file_server=None, max_text_bytes=24000):
    """建一个回包裁剪器。"""
    from gateway import ResponseShaper
    from tools import ToolContext

    return ResponseShaper(max_text_bytes=max_text_bytes,
                          context=ToolContext(file_server=file_server))


def make_service(tools=(), file_server=None):
    """建一个服务层，把给定的工具装进去。"""
    from gateway import McpService, ResponseShaper, ToolRegistry
    from gateway.runtime import EventLoop
    from tools import ToolContext

    context = ToolContext(file_server=file_server)
    registry = ToolRegistry(context)
    registry.add_tools(tools)
    shaper = ResponseShaper(context=context)
    runtime = EventLoop()
    return McpService(registry, shaper, runtime, context=context)


def payload_size(content):
    """裁好的内容装进回包之后有多少字节。"""
    from gateway import protocol

    payload = protocol.result(1, {"content": content, "isError": False})
    return len(json.dumps(payload).encode("utf-8"))


async def call_via_gateway(service, name, arguments):
    """走服务层调一个工具，返回回包里的 result。"""
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": name, "arguments": arguments}}
    response, _status = await service.handle(request)
    return (response or {}).get("result") or {}


async def check_shape(failures):
    """离线检查回包裁剪，不需要网关在跑。

    Windows-MCP 的截图回包带一张整屏 PNG，base64 有 780 KB。板端的接收缓冲
    只有 32 KB，超了整个调用就失败，所以网关要在发包前把回包裁到装得下。
    """
    shaper = make_shaper()
    cases = (
        ("screenshot", [
            {"type": "text", "text": "Screenshot taken." * 100},
            {"type": "image", "mimeType": "image/png", "data": "A" * 779012},
        ]),
        ("snapshot", [{"type": "text", "text": "window tree " * 3280}]),
    )
    for name, content in cases:
        shaped = await shaper.shape(content)
        size = payload_size(shaped)
        check(size <= DEVICE_BODY_LIMIT, "{} 回包 {} 字节，板端上限 {}".format(
            name, size, DEVICE_BODY_LIMIT), failures)
    shaped = await shaper.shape(cases[0][1])
    check(all(len(item.get("text", "")) < 4096 for item in shaped),
          "图片 base64 不发给设备端", failures)
    check(any("image omitted by gateway" in item.get("text", "") for item in shaped),
          "没接文件服务器时图片换成了一句说明", failures)


async def check_image_store(failures):
    """检查配了文件服务器之后，图片换成一条带下载地址的说明。

    这里用一个假的文件服务器，不真的上传，只看回包的形态。
    """
    server = FakeFileServer()
    shaper = make_shaper(server)
    shaped = await shaper.shape(
        [{"type": "image", "mimeType": "image/png", "data": base64.b64encode(b"abc").decode()}],
        "win__screenshot")
    note = shaped[0].get("text", "")
    check("image saved" in note and "/download/" in note,
          "图片换成带下载地址的说明", failures)
    check(server.last[0] == b"abc" and server.last[1] == "win__screenshot.png",
          "上传的是解出来的二进制，文件名带工具名", failures)
    size = payload_size(shaped)
    check(size <= DEVICE_BODY_LIMIT, "存盘后的回包 {} 字节，板端上限 {}".format(
        size, DEVICE_BODY_LIMIT), failures)


async def check_media_url(failures):
    """检查工具自己给的下载地址，网关原样带给板端。

    用户工具用 to_content(image=...)、to_content(audio=...) 存到文件服务器，
    回包里带的是 {"type": "image", "url": ...}，网关不能再改它。
    """
    shaper = make_shaper()
    shaped = await shaper.shape(
        [{"type": "image", "url": FAKE_URL}, {"type": "audio", "url": FAKE_URL}],
        "user__echo")
    check(len(shaped) == 2, "带地址的图片和音频都留下来了", failures)
    check(all(item.get("url") == FAKE_URL for item in shaped),
          "下载地址原样带给板端", failures)
    check(all(item.get("type") in ("image", "audio") for item in shaped),
          "类型还是 image 和 audio", failures)
    size = payload_size(shaped)
    check(size <= DEVICE_BODY_LIMIT, "带地址的回包 {} 字节，板端上限 {}".format(
        size, DEVICE_BODY_LIMIT), failures)


async def check_to_content(failures):
    """检查 to_content 的三种参数。

    文字直接给；图片和音频可以给文件路径或者原始字节，函数把文件存到文件
    服务器，回包里带的是下载地址。板端一次只收 32768 字节，整段 base64 上不去。
    """
    from tools import ToolContext, reset, use
    from tools.base import to_content

    server = FakeFileServer()
    token = use(ToolContext(file_server=server))
    try:
        content = await to_content(text="hello")
        check(content == [{"type": "text", "text": "hello"}], "to_content 只给文字", failures)

        content = await to_content(text="hello", image=b"\x89PNG\r\n\x1a\n")
        check(content[0] == {"type": "text", "text": "hello"}, "文字和图片可以一起给", failures)
        check(content[1].get("type") == "image" and content[1].get("url") == FAKE_URL,
              "图片存到文件服务器，回包里带的是地址", failures)
        check(server.last[0] == b"\x89PNG\r\n\x1a\n" and server.last[2] == "image/png",
              "上传的是原始字节，MIME 按类型定", failures)

        wav = b"RIFF\x24\x00\x00\x00WAVEfmt " + b"\x00" * 32
        content = await to_content(audio=wav)
        check(content[0].get("type") == "audio" and content[0].get("url") == FAKE_URL,
              "音频存到文件服务器，回包里带的是地址", failures)
        check(server.last[1] == "audio.wav" and server.last[2] == "audio/wav",
              "音频文件名的扩展名按 MIME 定", failures)

        handle, path = tempfile.mkstemp(suffix=".png")
        os.close(handle)
        try:
            with open(path, "wb") as image:
                image.write(b"\x89PNG\r\n\x1a\n")
            content = await to_content(image=path)
            check(content[0].get("url") == FAKE_URL, "本地图片文件存到文件服务器", failures)
            check(server.last[1].endswith(".png") and server.last[2] == "image/png",
                  "本地图片按扩展名定 MIME", failures)
        finally:
            os.remove(path)

        content = await to_content(image=FAKE_URL)
        check(content[0].get("url") == FAKE_URL, "http 地址直接用，不上传", failures)
    finally:
        reset(token)

    use(ToolContext(file_server=None))
    try:
        await to_content(audio=wav)
        check(False, "没配文件服务器时音频存不了", failures)
    except ValueError:
        check(True, "没配文件服务器时音频存不了", failures)


async def check_audio_store(failures):
    """检查网关收到外部 MCP 服务器的音频回包时怎么处理。

    音频和图片走同一条路：base64 解出来传上去，回包里只留一句说明。
    """
    server = FakeFileServer()
    shaper = make_shaper(server)
    wav = b"RIFF\x24\x00\x00\x00WAVEfmt " + b"\x00" * 32
    shaped = await shaper.shape(
        [{"type": "audio", "mimeType": "audio/wav",
          "data": base64.b64encode(wav).decode()}],
        "user__audio")
    note = shaped[0].get("text", "")
    check("audio saved" in note and "/download/" in note,
          "音频换成带下载地址的说明", failures)
    check(server.last[0] == wav, "上传的是解出来的音频二进制", failures)
    check(server.last[1] == "user__audio.wav", "音频文件名的扩展名按 MIME 定", failures)
    check(server.last[2] == "audio/wav", "上传时带上音频的 MIME", failures)

    size = payload_size(shaped)
    check(size <= DEVICE_BODY_LIMIT, "音频回包 {} 字节，板端上限 {}".format(
        size, DEVICE_BODY_LIMIT), failures)

    # 没配文件服务器时退回占位说明，板端照样收得下
    plain = make_shaper()
    shaped = await plain.shape(
        [{"type": "audio", "mimeType": "audio/wav",
          "data": base64.b64encode(wav).decode()}],
        "user__audio")
    check("audio omitted by gateway" in shaped[0].get("text", ""),
          "没配文件服务器时音频换成了一句说明", failures)

    # 一整段 100 KB 的音频也不该把回包撑爆
    big = base64.b64encode(b"\x00" * 100000).decode()
    shaped = await plain.shape(
        [{"type": "audio", "mimeType": "audio/wav", "data": big}],
        "user__audio")
    size = payload_size(shaped)
    check(size <= DEVICE_BODY_LIMIT, "100 KB 音频的回包 {} 字节，板端上限 {}".format(
        size, DEVICE_BODY_LIMIT), failures)


async def check_user_tools(failures):
    """离线检查用户工具的发现、注册和调用，不需要网关在跑。

    用户工具是继承基类自己写的类，放在 tools/user_tools 目录里，一个模块一个
    工具类。网关启动时自动发现，这里验证这条链路本身是对的。
    """
    from tools import USER_TOOL_DIR, load_user_tools
    from tools.base import Tool

    tools = load_user_tools()
    names = [tool.name for tool in tools]
    check("echo" in names, "自动发现 tools/user_tools 目录里的工具", failures)
    check("now" in names, "不同模块里的工具都注册了", failures)

    schema = tools[0].schema() if len(tools) else {}
    check(bool(schema.get("name")) and bool(schema.get("description"))
          and isinstance(schema.get("inputSchema"), dict),
          "工具描述含 name、description、inputSchema", failures)

    exposed = [tool.exposed_name for tool in tools]
    check("user__echo" in exposed, "对外暴露的名字带 user__ 前缀", failures)

    echo = next(tool for tool in tools if tool.name == "echo")
    content = await echo.execute(text="hello")
    text = (content or [{}])[0].get("text", "")
    check(text == "hello", "调用用户工具返回结果", failures)

    # 一个模块一个工具类
    multi = []
    for entry in sorted(os.listdir(USER_TOOL_DIR)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        path = os.path.join(USER_TOOL_DIR, entry)
        with open(path, "r", encoding="utf-8") as source:
            body = source.read()
        count = body.count("(Tool)") + body.count("(Tool, ")
        if count > 1:
            multi.append(entry)
    check(not multi, "一个模块只写一个工具类（多写的：{}）".format(", ".join(multi)), failures)

    class BrokenTool(Tool):
        @property
        def name(self):
            return "broken"

        @property
        def description(self):
            return "always fails"

        @property
        def parameters(self):
            return {"type": "object", "properties": {}}

        async def execute(self, **kwargs):
            raise RuntimeError("boom")

    service = make_service([BrokenTool()])
    result = await call_via_gateway(service, "broken", {})
    check(bool(result.get("isError")), "工具内部抛错按失败返回", failures)
    check("boom" in (result.get("content") or [{}])[0].get("text", ""),
          "失败原因写进 content", failures)

    # 同名的工具只留一个
    with tempfile.TemporaryDirectory() as directory:
        for index in range(2):
            path = os.path.join(directory, "dup{}.py".format(index))
            with open(path, "w", encoding="utf-8") as source:
                source.write(
                    "from tools.base import Tool\n\n\n"
                    "class DupTool(Tool):\n"
                    "    @property\n"
                    "    def name(self):\n"
                    "        return 'dup'\n\n"
                    "    @property\n"
                    "    def description(self):\n"
                    "        return 'same name'\n\n"
                    "    @property\n"
                    "    def parameters(self):\n"
                    "        return {'type': 'object', 'properties': {}}\n\n"
                    "    async def execute(self, **kwargs):\n"
                    "        return []\n")
        dups = load_user_tools(directory)
        check(len(dups) == 1, "同名工具只留一个", failures)


async def check_web_search(failures):
    """离线检查联网搜索工具。

    搜索服务不可达时工具要给出失败原因，不能让连接挂掉。各家搜索服务的回包
    形状不一样，这里检查三种常见形状都能解出结果。
    """
    from tools.user_tools.web_search_tool import (NAME, WebSearchTool,
                                                   detect_mode, extract_results)

    tool = WebSearchTool(url="http://127.0.0.1:9/search")
    schema = tool.schema()
    check(schema.get("name") == NAME, "web_search 注册成一个工具", failures)

    properties = (schema.get("inputSchema") or {}).get("properties") or {}
    check("query" in properties, "web_search 的参数表里有 query", failures)
    check((schema.get("inputSchema") or {}).get("required") == ["query"],
          "query 是必填参数", failures)

    reply = await tool.execute()
    check(bool(reply.get("isError")), "没有 query 时按失败返回", failures)

    reply = await tool.execute(query="rt-thread")
    check(bool(reply.get("isError")), "搜索服务不可达时按失败返回", failures)
    check(bool((reply.get("content") or [{}])[0].get("text")),
          "失败原因写进 content", failures)

    blank = WebSearchTool(url="")
    reply = await blank.execute(query="rt-thread")
    check(bool(reply.get("isError")), "没配搜索服务地址时按失败返回", failures)

    bing = {"webPages": {"value": [
        {"name": "标题", "url": "https://a.example", "snippet": "<b>摘要</b>"}]}}
    results = extract_results(bing, 5)
    check(len(results) == 1 and results[0]["title"] == "标题",
          "Bing 形状的结果能认出来", failures)
    check(results[0]["snippet"] == "摘要", "摘要里的标签被清掉了", failures)

    tavily = {"results": [{"title": "T", "url": "https://b.example",
                            "content": "正文"}]}
    check(len(extract_results(tavily, 5)) == 1,
          "Tavily 形状的结果能认出来", failures)

    searx = {"results": [{"title": "T", "url": "https://c.example",
                          "content": "正文"}],
             "suggestions": ["关键词"]}
    check(len(extract_results(searx, 5)) == 1, "SearXNG 形状的结果能认出来", failures)
    check(len(extract_results(searx, 1)) == 1, "结果条数按参数收口", failures)

    check(detect_mode("https://searx.be/search") == "searxng",
          "地址里带 searx 就按 SearXNG 对接", failures)
    check(detect_mode("https://api.bing.microsoft.com/v7.0/search") == "bing",
          "地址里带 bing 就按 Bing 对接", failures)
    check(detect_mode("https://api.example.com/search") == "post",
          "认不出来的地址按通用 JSON 接口对接", failures)

    tool = WebSearchTool(url="http://127.0.0.1:9/search",
                          fetch=lambda query, count: {
                              "results": [{"title": "T", "url": "https://d.example",
                                           "content": "正文"}]})
    reply = await tool.execute(query="rt-thread")
    text = (reply.get("content") or [{}])[0].get("text", "")
    check(not reply.get("isError") and "https://d.example" in text,
          "回包里带的是整理好的结果", failures)


async def check_config(failures):
    """离线检查配置层和工具的 configure 钩子。

    命令行和环境变量在 config 层解析一次，工具注册的时候通过
    configure(context) 拿到配置。工具自己不读环境变量。
    """
    from config import parse_args
    from gateway import ToolRegistry
    from tools import ToolContext
    from tools.user_tools.web_search_tool import DEFAULT_URL, WebSearchTool

    with env(MCP_WEB_SEARCH_ENABLED="1", MCP_WEB_SEARCH_URL=None):
        settings = parse_args([])
        check(settings.get("port") == 8765, "默认端口 8765", failures)
        check(settings.get("web_search.enabled") is True, "默认接入联网搜索", failures)
        check(settings.get("web_search.url") == DEFAULT_URL,
              "联网搜索有默认地址", failures)

        settings = parse_args(["--no-web-search"])
        check(settings.get("web_search.enabled") is False,
              "--no-web-search 关掉联网搜索", failures)

        settings = parse_args(["--web-search-url", "http://127.0.0.1:9/search"])
        check(settings.get("web_search.url") == "http://127.0.0.1:9/search",
              "命令行能改搜索服务地址", failures)

    with env(MCP_WEB_SEARCH_URL="http://127.0.0.1:8/search",
             MCP_WEB_SEARCH_ENABLED="1"):
        settings = parse_args([])
        check(settings.get("web_search.url") == "http://127.0.0.1:8/search",
              "环境变量能改搜索服务地址", failures)

    # 工具注册的时候从上下文里读配置
    context = ToolContext(settings=settings)
    tool = WebSearchTool()
    tool.configure(context)
    check(tool.url == "http://127.0.0.1:8/search",
          "工具从上下文里读到搜索服务地址", failures)

    # 说不能用就不注册
    off = ToolRegistry(ToolContext(settings=parse_args(["--no-web-search"])))
    check(not off.add_tools([WebSearchTool()]), "关掉之后工具不注册", failures)

    on = ToolRegistry(ToolContext(settings=settings))
    accepted = on.add_tools([WebSearchTool()])
    check(len(accepted) == 1 and accepted[0].exposed_name == "web_search",
          "开着的时候工具按 web_search 注册", failures)


async def check_offline(failures):
    """所有不需要网关在跑的检查。"""
    await check_shape(failures)
    await check_image_store(failures)
    await check_media_url(failures)
    await check_to_content(failures)
    await check_audio_store(failures)
    await check_user_tools(failures)
    await check_web_search(failures)
    await check_config(failures)


def main():
    parser = argparse.ArgumentParser(description="MCP 网关自测")
    parser.add_argument("--url", default="http://127.0.0.1:8765/mcp", help="网关地址")
    parser.add_argument("--root", default=None, help="用于测试 read_text_file 的文件")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    client = Client(args.url, args.timeout)
    failures = []

    asyncio.run(check_offline(failures))

    reply = client.request("initialize", {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {},
        "clientInfo": {"name": "mcp-gateway-test", "version": "1.0.0"},
    })
    result = reply.get("result") or {}
    check(result.get("protocolVersion") == PROTOCOL_VERSION, "initialize 返回协议版本", failures)
    check(bool(result.get("serverInfo")), "initialize 返回 serverInfo", failures)
    check(bool(client.session_id), "服务端下发 Mcp-Session-Id", failures)

    client.notify("notifications/initialized")
    check(True, "notifications/initialized 返回 202", failures)

    reply = client.request("tools/list")
    tools = (reply.get("result") or {}).get("tools") or []
    check(bool(tools), "tools/list 返回工具清单（{} 个）".format(len(tools)), failures)
    for tool in tools:
        print("       - {}: {}".format(tool.get("name"), tool.get("description", "")))

    names = [tool.get("name") for tool in tools]
    if "read_text_file" in names and args.root:
        reply = client.request("tools/call", {
            "name": "read_text_file",
            "arguments": {"filePath": args.root},
        })
        content = (reply.get("result") or {}).get("content") or []
        text = content[0].get("text", "") if content else ""
        check(bool(text), "read_text_file 返回内容", failures)
    elif "get_directories" in names:
        reply = client.request("tools/call", {
            "name": "get_directories",
            "arguments": {"path": "."},
        })
        content = (reply.get("result") or {}).get("content") or []
        check(bool(content), "get_directories 返回目录列表", failures)
    else:
        reply = None
    if reply is not None:
        check(client.last_size <= DEVICE_BODY_LIMIT,
              "tools/call 回包 {} 字节，板端上限 {}".format(
                  client.last_size, DEVICE_BODY_LIMIT), failures)

    if "user__echo" in names:
        reply = client.request("tools/call", {
            "name": "user__echo",
            "arguments": {"text": "hello from test"},
        })
        content = (reply.get("result") or {}).get("content") or []
        text = content[0].get("text", "") if content else ""
        check(text == "hello from test", "tools/call 能调通用户工具", failures)

    if "web_search" in names:
        reply = client.request("tools/call", {
            "name": "web_search",
            "arguments": {"query": "RT-Thread"},
        })
        content = (reply.get("result") or {}).get("content") or []
        text = content[0].get("text", "") if content else ""
        check(bool(text), "tools/call 能调通联网搜索", failures)
        check(client.last_size <= DEVICE_BODY_LIMIT,
              "联网搜索回包 {} 字节，板端上限 {}".format(
                  client.last_size, DEVICE_BODY_LIMIT), failures)

    if "user__audio" in names:
        reply = client.request("tools/call", {
            "name": "user__audio",
            "arguments": {},
        })
        content = (reply.get("result") or {}).get("content") or []
        audio = [item for item in content if item.get("type") == "audio"]
        check(bool(audio), "tools/call 的音频回包带音频条目", failures)
        check(bool(audio) and audio[0].get("url", "").startswith("http"),
              "音频条目带的是文件服务器的地址", failures)
        check(client.last_size <= DEVICE_BODY_LIMIT,
              "音频回包 {} 字节，板端上限 {}".format(
                  client.last_size, DEVICE_BODY_LIMIT), failures)

    if failures:
        print("自测失败：{} 项未通过".format(len(failures)))
        return 1
    print("自测通过")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (urllib.error.URLError, OSError, RuntimeError, ValueError) as exc:
        print("自测失败：{}".format(exc))
        sys.exit(1)
