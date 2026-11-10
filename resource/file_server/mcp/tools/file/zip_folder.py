"""把目录打包成 zip。"""
import os
import zipfile

from ..base import Tool
from .paths import resolve, run_tool, string_prop


class ZipFolderTool(Tool):
    """把目录打包成 zip。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "zip_folder"

    @property
    def description(self):
        return "Zip a folder into a zip file. Creates a new file."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "folderPath": string_prop("folderPath"),
                "zipName": string_prop("zipName"),
            },
            "required": ["folderPath", "zipName"],
        }

    async def execute(self, **kwargs):
        """把 folderPath 打包成 folderPath 下的 zipName。压缩包自身不进包。"""
        return await run_tool(self._zip, kwargs)

    def _zip(self, args):
        folder = resolve(self.root, args.get("folderPath"))
        zip_name = args.get("zipName") or ""
        if not zip_name.lower().endswith(".zip"):
            zip_name += ".zip"
        target = os.path.join(folder, zip_name)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
            for base, _dirs, files in os.walk(folder):
                for filename in files:
                    full = os.path.join(base, filename)
                    if os.path.abspath(full) == os.path.abspath(target):
                        continue
                    archive.write(full, os.path.relpath(full, folder))
        return {"path": target}
