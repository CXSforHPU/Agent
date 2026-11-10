"""按文件名模式搜索文件。"""
import os

from ..base import Tool
from .paths import match, resolve, run_tool, snippet, string_prop, timestamp


class SearchFilesTool(Tool):
    """按文件名模式搜索文件。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "search_files"

    @property
    def description(self):
        return "Search files by name pattern under a directory. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "startingDir": string_prop("startingDir"),
                "searchPatterns": string_prop("searchPatterns"),
                "specifiedFileTypes": string_prop("specifiedFileTypes"),
                "startDate": string_prop("startDate"),
                "endDate": string_prop("endDate"),
            },
            "required": ["startingDir", "searchPatterns"],
        }

    async def execute(self, **kwargs):
        """递归找文件名里带模式的文件。模式和类型都用分号隔开。"""
        return await run_tool(self._search, kwargs)

    def _search(self, args):
        start = resolve(self.root, args.get("startingDir"))
        patterns = str(args.get("searchPatterns") or "").split(";")
        patterns = [item.strip() for item in patterns if item.strip()]
        types = str(args.get("specifiedFileTypes") or "").split(";")
        types = [item.strip().lower().lstrip(".") for item in types if item.strip()]
        found = []
        for base, _dirs, files in os.walk(start):
            for filename in files:
                if not match(filename, patterns):
                    continue
                if types and os.path.splitext(filename)[1].lstrip(".").lower() not in types:
                    continue
                full = os.path.join(base, filename)
                stat = os.stat(full)
                found.append({
                    "name": filename,
                    "path": full,
                    "dateCreated": timestamp(stat.st_ctime),
                    "snippet": snippet(full),
                })
        return {"result": found}
