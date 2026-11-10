"""移动或改名文件。"""
import shutil

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class MoveFileTool(Tool):
    """移动或改名文件。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "move_file"

    @property
    def description(self):
        return "Move or rename a file. Changes the file system."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "oldFullPath": string_prop("oldFullPath"),
                "newFullPath": string_prop("newFullPath"),
            },
            "required": ["oldFullPath", "newFullPath"],
        }

    async def execute(self, **kwargs):
        """把 oldFullPath 移到 newFullPath。目标可以不存在，父目录要存在。"""
        return await run_tool(self._move, kwargs)

    def _move(self, args):
        source = resolve(self.root, args.get("oldFullPath"))
        target = resolve(self.root, args.get("newFullPath"), must_exist=False)
        shutil.move(source, target)
        return {"result": target}
