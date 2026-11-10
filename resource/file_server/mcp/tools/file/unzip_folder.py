"""把 zip 解压到目录。"""
import os
import zipfile

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class UnzipFolderTool(Tool):
    """把 zip 解压到目录。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "unzip_folder"

    @property
    def description(self):
        return "Extract a zip file into a folder. Writes files."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "zipPath": string_prop("zipPath"),
                "extractPath": string_prop("extractPath"),
            },
            "required": ["zipPath", "extractPath"],
        }

    async def execute(self, **kwargs):
        """解压 zipPath 到 extractPath。包里的路径要越界时直接报错，不落盘。"""
        return await run_tool(self._unzip, kwargs)

    def _unzip(self, args):
        archive_path = resolve(self.root, args.get("zipPath"))
        target = resolve(self.root, args.get("extractPath"), must_exist=False)
        os.makedirs(target, exist_ok=True)
        with zipfile.ZipFile(archive_path) as archive:
            for member in archive.namelist():
                full = os.path.realpath(os.path.join(target, member))
                if full != target and not full.startswith(target + os.sep):
                    raise ValueError("zip entry escapes the extract path: {}".format(member))
            archive.extractall(target)
        return {"path": target}
