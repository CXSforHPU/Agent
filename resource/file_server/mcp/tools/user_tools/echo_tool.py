"""把传进来的文字原样返回。

这是用户工具的样板：一个模块一个工具类，继承基类实现四个成员就行。照着
抄一份改成自己的工具，放进同一个目录，网关不用改。
"""
from tools.base import Tool, to_content


class EchoTool(Tool):
    """把传进来的文字原样返回。"""

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

    async def execute(self, text="", **kwargs):
        return await to_content(text=text)
