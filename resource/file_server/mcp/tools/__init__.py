"""工具框架。

工具只有一种：Tool。内置文件工具、联网搜索、外部 MCP 服务器、用户自己的
工具，最后都是一串 Tool，网关不分来源。

写一个继承 Tool 的类，放进 tools/user_tools/ 目录，一个模块一个工具类，网关
启动时自动发现并注册。设备端和别的 MCP 工具一样能调它们，名字前面多一段
user__。

    from tools.base import Tool, to_content


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

文件里以 _ 开头的不会被加载。

工具要读配置就实现 configure(context)，网关注册的时候调一次。工具不 import
网关，也不读写模块级全局变量，要什么从 current_context() 里拿。
"""
from .base import Tool, as_result, format_text, to_content
from .context import ToolContext, current_context, reset, use
from .loader import USER_TOOL_DIR, load_user_tools

__all__ = ["Tool", "to_content", "format_text", "as_result", "ToolContext",
           "current_context", "use", "reset", "USER_TOOL_DIR", "load_user_tools"]
