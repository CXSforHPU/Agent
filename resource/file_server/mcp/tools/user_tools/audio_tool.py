"""返回一段 WAV 音频。

音频交给 to_content，函数把它存到文件服务器，回包里带的是下载地址。板端
一次只收 32768 字节，整段 base64 上不去。
"""
import math
import struct

from tools.base import Tool, to_content


class AudioTool(Tool):
    """返回一段 WAV 音频。"""

    @property
    def name(self):
        return "audio"

    @property
    def description(self):
        return "Return a short WAV clip as audio content. Read-only."

    @property
    def parameters(self):
        return {"type": "object", "properties": {}}

    async def execute(self, **kwargs):
        return await to_content(audio=_wav_bytes())


def _wav_bytes():
    """造一段 0.1 秒的 440 Hz 单声道 WAV，返回原始字节。

    16 位 PCM，8 kHz。文件服务器靠 RIFF/WAVE 魔数认出它是 WAV。
    """
    rate = 8000
    pcm = b"".join(
        struct.pack("<h", int(6000 * math.sin(2 * math.pi * 440 * i / rate)))
        for i in range(rate // 10))
    header = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
              + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
              + b"data" + struct.pack("<I", len(pcm)))
    return header + pcm
