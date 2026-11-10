"""读取文本文件的内容。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class ReadTextFileTool(Tool):
    """读取文本文件的内容。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "read_text_file"

    @property
    def description(self):
        return "Read the text content of a file. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {"filePath": string_prop("filePath")},
            "required": ["filePath"],
        }

    async def execute(self, **kwargs):
        """返回文件的全文。解码失败的字节按替换符处理。"""
        return await run_tool(self._read, kwargs)

    def _read(self, args):
        target = resolve(self.root, args.get("filePath"))
        if not os.path.isfile(target):
            raise ValueError("not a file: {}".format(args.get("filePath")))
        with open(target, "r", encoding="utf-8", errors="replace") as handle:
            return {"result": handle.read()}
