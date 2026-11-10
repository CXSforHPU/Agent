"""列出目录下的子目录。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class GetDirectoriesTool(Tool):
    """列出目录下的子目录。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "get_directories"

    @property
    def description(self):
        return "List the directories under a path. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {"path": string_prop("path")},
            "required": ["path"],
        }

    async def execute(self, **kwargs):
        """返回目录下的子目录列表，按名字排序。"""
        return await run_tool(self._list, kwargs)

    def _list(self, args):
        target = resolve(self.root, args.get("path"))
        entries = []
        for item in sorted(os.listdir(target)):
            full = os.path.join(target, item)
            if os.path.isdir(full):
                entries.append({"path": full})
        return {"result": entries}
