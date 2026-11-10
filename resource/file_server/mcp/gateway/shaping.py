"""回包裁剪。

板端一次最多收 32768 字节的响应体，超了整个调用就失败。Windows-MCP 的截图
回包带一张整屏 PNG，base64 有 780 KB，板端收不下。

这一层在发包前把回包压到装得下：图片和音频存到文件服务器，回包里只留一句带
下载地址的说明，base64 不上线；剩下的文字按预算截断，丢掉的部分在结尾说明，
板端和模型都知道回包被裁过。
"""
import asyncio
import base64
import json

from tools.context import ToolContext
from tools.media import DEFAULT_MIME, media_name

# 单次回包里文本的字节上限，要小于板端的 PKG_AGENT_MCP_RESP_BUFSZ
DEFAULT_MAX_TEXT_BYTES = 24000

TRUNCATE_NOTE = "\n[truncated {n} more chars]"


class ResponseShaper:
    """把工具回包裁到板端收得下的尺寸。"""

    def __init__(self, max_text_bytes=DEFAULT_MAX_TEXT_BYTES, context=None):
        self.max_text_bytes = max_text_bytes
        self._context = context or ToolContext()

    async def shape(self, content, tool_name=""):
        """裁剪工具回包的内容。"""
        shaped = []
        used = 0

        for item in content:
            if not isinstance(item, dict):
                continue
            kind = item.get("type")
            if kind in ("image", "audio"):
                url = item.get("url")
                if isinstance(url, str) and url:
                    # 工具已经把文件存到文件服务器了，地址直接带给板端
                    media_item = {"type": kind, "url": url}
                    shaped.append(media_item)
                    used += len(json.dumps(media_item))
                    continue
                note = await self._store_media(item, tool_name, kind)
                shaped.append({"type": "text", "text": note})
                used += len(json.dumps(note))
                continue
            if kind != "text":
                continue
            text = item.get("text")
            if not isinstance(text, str):
                continue
            room = self.max_text_bytes - used
            if room <= 0:
                break
            clipped = self._clip_text(text, room)
            shaped.append({"type": "text", "text": clipped})
            used += len(json.dumps(clipped))
        return shaped

    @staticmethod
    def _clip_text(text, budget):
        """把一段文本截到预算以内，结尾说明丢了多少字符。

        预算按回包里的字节数算，才能跟板端的接收缓冲对得上。JSON 转义会把
        非 ASCII 字符放大，所以用转义后的长度衡量，而不是字符数。
        """
        if len(json.dumps(text)) <= budget:
            return text
        room = budget - len(json.dumps(TRUNCATE_NOTE.format(n=0)))
        if room < 0:
            room = 0
        low, high = 0, len(text)
        while low < high:
            mid = (low + high + 1) // 2
            if len(json.dumps(text[:mid])) <= room:
                low = mid
            else:
                high = mid - 1
        return text[:low] + TRUNCATE_NOTE.format(n=len(text) - low)

    async def _store_media(self, item, tool_name, kind):
        """把图片或音频存到文件服务器，换成一句带下载地址的说明。

        没配文件服务器，或者上传失败，就退回一句占位说明，板端照样收得下。
        """
        data = item.get("data") or ""
        mime = item.get("mimeType") or DEFAULT_MIME.get(kind, "application/octet-stream")
        note = "[{} omitted by gateway: {}, {} base64 chars]".format(kind, mime, len(data))
        server = self._context.file_server
        if not server or not data:
            return note
        try:
            raw = base64.b64decode(data)
        except (ValueError, TypeError):
            return note
        url = await asyncio.to_thread(server.upload, raw,
                                     media_name(tool_name, mime), mime)
        if not url:
            return note
        return "[{} saved: {} ({}, {} bytes)]".format(kind, url, mime, len(raw))
