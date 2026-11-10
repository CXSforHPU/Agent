"""工具基类。

所有工具都继承这个类，写一个模块一个工具类：

    class EchoTool(Tool):
        @property
        def name(self):
            return "echo"

        @property
        def description(self):
            return "Echo the given text back. Read-only."

        @property
        def parameters(self):
            return {
                "type": "object",
                "properties": {"text": {"type": "string", "title": "text"}},
                "required": ["text"],
            }

        async def execute(self, text=""):
            return await to_content(text=text)

类要放进 tools/user_tools/ 目录，网关启动时自动发现并注册，不用改网关的代码。
内置的文件工具在 tools/file/ 目录，联网搜索在 tools/user_tools/web_search_tool.py，
外部 MCP 服务器报上来的工具用 RemoteTool。

返回结果用 to_content：文字直接给，图片和音频给文件路径或者字节，函数把文件
存到文件服务器，回包里带的是下载地址。execute 也可以直接返回一段文字，或者
带 isError 的完整结果。

工具要读配置就实现 configure(context)：网关注册的时候调一次，context 里有
文件服务器和网关配置。execute 里用 current_context() 取当前的上下文。
"""
import asyncio
import json

from .media import store_media


class Tool:
    """一个能被设备端调用的工具。

    子类实现 name、description、parameters、execute 四个成员。execute 写成
    async def，工具里有阻塞的活儿就用 await asyncio.to_thread(...) 丢到线程里。

    两个名字前缀：

    - namespace：一直带在对外名字前面，用来把不同来源的工具分开。
    - prefix：只在对外名字撞车时用上，撞车的那个工具改用 prefix 加原名。

    类里写 namespace = None 表示交给网关决定：放在 user_tools 目录里的工具
    默认是 user__，内置的工具在自己的类里写死。
    """

    namespace = None
    prefix = None

    # 工具自己判断要不要注册，条件不满足时置 False，网关跳过
    enabled = True

    def __init__(self, name=None, description=None, parameters=None,
                 namespace=None, prefix=None, closer=None):
        self._name = name or ""
        self._description = description or ""
        self._parameters = parameters or {"type": "object", "properties": {}}
        self._closer = closer
        if namespace is not None:
            self.namespace = namespace
        if prefix is not None:
            self.prefix = prefix

    def configure(self, context):
        """注册时把上下文交给工具。

        工具要读配置就在这里读，之后 execute 里不用再关心配置从哪来。默认
        什么都不做。
        """
        return None

    @property
    def name(self):
        """工具名。字母、数字、下划线，和别的工具重了由网关加前缀。"""
        return self._name

    @property
    def description(self):
        """工具说明。这段文字会发给模型，写清楚这个工具做什么。"""
        return self._description

    @property
    def parameters(self):
        """参数表，JSON Schema，和 tools/list 里的 inputSchema 一样。"""
        return self._parameters

    @property
    def exposed_name(self):
        """对外的名字。namespace 加工具名。"""
        return (self.namespace or "") + self.name

    async def execute(self, **kwargs):
        """执行工具。子类实现这个方法。

        返回一段文字，或者一个 MCP 的 content 数组，也可以带 isError 的
        完整结果。用 to_content 包结果。
        """
        raise NotImplementedError("工具 {} 没有实现 execute".format(self.name))

    def schema(self):
        """工具的描述，给网关的 tools/list 用。"""
        return {
            "name": self.exposed_name,
            "description": self.description,
            "inputSchema": self.parameters,
        }

    def close(self):
        """释放工具持有的资源，默认没有资源可释放。"""
        closer = getattr(self, "_closer", None)
        if closer:
            closer()


async def to_content(text=None, image=None, audio=None):
    """把工具的结果包成 MCP 的 content 数组。

    text 是要回给模型的文字。image 和 audio 可以是本地文件路径、原始字节，
    或者 http 地址。本地文件和字节会存到文件服务器，回包里带的是文件服务器
    的下载地址：板端一次只收 32768 字节，整段 base64 上不去。
    """
    content = []
    if text is not None:
        content.append(format_text(text))
    if image is not None:
        content.append({"type": "image", "url": await store_media("image", image)})
    if audio is not None:
        content.append({"type": "audio", "url": await store_media("audio", audio)})
    return content


def format_text(text):
    """一条文字。"""
    return {"type": "text", "text": text}


def as_result(value):
    """把工具的返回值统一成 (content 数组, 是否失败)。

    content 数组直接用，字符串当一条文字，其他字典按 JSON 文本给。
    """
    if value is None:
        return [], False
    if isinstance(value, dict) and isinstance(value.get("content"), list):
        return value["content"], bool(value.get("isError"))
    if isinstance(value, list):
        return value, False
    if isinstance(value, str):
        return [format_text(value)], False
    return [format_text(json.dumps(value, ensure_ascii=False))], False
