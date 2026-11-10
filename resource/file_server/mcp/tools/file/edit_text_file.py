"""改文件里的一段文字。"""
from ..base import Tool
from .paths import resolve, run_tool, string_prop


class EditTextFileTool(Tool):
    """改文件里的一段文字。"""

    namespace = ""
    prefix = "file__"

    def __init__(self, root):
        super().__init__()
        self.root = root

    @property
    def name(self):
        return "edit_text_file"

    @property
    def description(self):
        return "Replace old text with new text in a file. Changes the file system."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {
                "filePath": string_prop("filePath"),
                "oldText": string_prop("oldText"),
                "newText": string_prop("newText"),
            },
            "required": ["filePath", "oldText", "newText"],
        }

    async def execute(self, **kwargs):
        """把文件里第一处 oldText 换成 newText。oldText 为空时整个文件被替换。"""
        return await run_tool(self._edit, kwargs)

    def _edit(self, args):
        target = resolve(self.root, args.get("filePath"))
        old_text = args.get("oldText") or ""
        new_text = args.get("newText") or ""
        with open(target, "r", encoding="utf-8", errors="replace") as handle:
            content = handle.read()
        if old_text and old_text not in content:
            raise ValueError("oldText not found in the file")
        updated = content.replace(old_text, new_text, 1) if old_text else new_text
        with open(target, "w", encoding="utf-8") as handle:
            handle.write(updated)
        return {"result": target}
