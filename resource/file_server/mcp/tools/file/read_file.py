"""以资源的形式读取文件内容。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class ReadFileTool(Tool):
    """以资源的形式读取文件内容。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "read_file"

    @property
    def description(self):
        return "Read the content of a file as a resource. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {"filePath": string_prop("filePath")},
            "required": ["filePath"],
        }

    async def execute(self, **kwargs):
        """返回文件内容。UTF-8 解不了就按 latin-1 处理，保证每个字节都不丢。"""
        return await run_tool(self._read, kwargs)

    def _read(self, args):
        target = resolve(self.root, args.get("filePath"))
        if not os.path.isfile(target):
            raise ValueError("not a file: {}".format(args.get("filePath")))
        with open(target, "rb") as handle:
            data = handle.read()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("latin-1")
        return {"contents": [{"type": "text", "text": text}], "_meta": None}
