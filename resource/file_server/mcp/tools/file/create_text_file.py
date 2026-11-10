"""新建文本文件。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class CreateTextFileTool(Tool):
    """新建文本文件。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "create_text_file"

    @property
    def description(self):
        return "Create a text file with optional content. Changes the file system."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "path": string_prop("path"),
                "filename": string_prop("filename"),
                "content": string_prop("content"),
            },
            "required": ["path", "filename"],
        }

    async def execute(self, **kwargs):
        """在 path 下建一个 filename 文件，写入 content。已有的文件会被覆盖。"""
        return await run_tool(self._create, kwargs)

    def _create(self, args):
        parent = resolve(self.root, args.get("path"))
        filename = args.get("filename") or ""
        if not filename or os.sep in filename or (os.altsep and os.altsep in filename):
            raise ValueError("invalid filename")
        target = os.path.join(parent, filename)
        with open(target, "w", encoding="utf-8") as handle:
            handle.write(args.get("content") or "")
        return {"result": target}
