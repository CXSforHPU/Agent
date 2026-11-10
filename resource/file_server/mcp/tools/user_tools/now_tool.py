"""返回主机当前的本地时间。"""
import datetime

from tools.base import Tool, to_content


class NowTool(Tool):
    """返回主机当前的本地时间。"""

    @property
    def name(self):
        return "now"

    @property
    def description(self):
        return "Return the current local time of the host. Read-only."

    @property
    def parameters(self):
        return {"type": "object", "properties": {}}

    async def execute(self, **kwargs):
        return await to_content(text=datetime.datetime.now().isoformat(timespec="seconds"))
