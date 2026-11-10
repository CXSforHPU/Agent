"""获取单个文件的元信息。"""
import os

from ..base import Tool
from .paths import resolve, run_tool, string_prop, timestamp


class GetFileDetailsTool(Tool):
    """获取单个文件的元信息。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "get_file_details"

    @property
    def description(self):
        return "Get metadata of a single file. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {"path": string_prop("path")},
            "required": ["path"],
        }

    async def execute(self, **kwargs):
        """返回文件的大小、扩展名和三个时间戳。"""
        return await run_tool(self._details, kwargs)

    def _details(self, args):
        target = resolve(self.root, args.get("path"))
        if not os.path.isfile(target):
            raise ValueError("not a file: {}".format(args.get("path")))
        stat = os.stat(target)
        return {
            "extension": os.path.splitext(target)[1] or None,
            "size": stat.st_size,
            "creationTime": timestamp(stat.st_ctime),
            "lastAccessTime": timestamp(stat.st_atime),
            "lastWriteTime": timestamp(stat.st_mtime),
        }
