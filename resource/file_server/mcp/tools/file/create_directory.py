"""新建目录。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class CreateDirectoryTool(Tool):
    """新建目录。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "create_directory"

    @property
    def description(self):
        return "Create a new directory. Changes the file system."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "path": string_prop("path"),
                "directoryName": string_prop("directoryName"),
            },
            "required": ["path", "directoryName"],
        }

    async def execute(self, **kwargs):
        """在 path 下建一个 directoryName 目录。目录名里不能带路径分隔符。"""
        return await run_tool(self._create, kwargs)

    def _create(self, args):
        parent = resolve(self.root, args.get("path"))
        name = args.get("directoryName") or ""
        if not name or os.sep in name or (os.altsep and os.altsep in name):
            raise ValueError("invalid directoryName")
        target = os.path.join(parent, name)
        os.makedirs(target, exist_ok=True)
        return {"result": target}
