"""媒体与文件服务器。

板端一次最多收 32768 字节的响应体，整屏截图的 base64 有 780 KB，直接发过去
整个调用就失败。图片和音频先存到文件服务器，回包里只留一个下载地址，板端和
模型都拿得到这个地址。

网关和用户工具共用这一份实现：

- 网关收到外部 MCP 服务器（Windows-MCP 等）回的 base64 媒体，存到文件服务器，
  再把回包换成一句带地址的说明。
- 用户工具用 to_content(image=...)、to_content(audio=...)，把文件交进来，
  拿到的就是文件服务器的地址。

文件服务器从上下文里取，不放在模块级全局变量里。
"""
import asyncio
import json
import mimetypes
import os
import urllib.request
import uuid

from .context import current_context

# 工具没给 mimeType 时，图片和音频各按这个类型存
DEFAULT_MIME = {"image": "image/png", "audio": "audio/wav"}

# 上传时按 MIME 的子类型定扩展名，文件服务器靠扩展名和魔数判断真实类型
MEDIA_SUFFIX = {
    "png": "png", "jpg": "jpg", "jpeg": "jpg", "gif": "gif", "webp": "webp",
    "bmp": "bmp", "wav": "wav", "x-wav": "wav", "wave": "wav", "mp3": "mp3",
    "mpeg": "mp3", "aac": "aac", "ogg": "ogg", "opus": "ogg", "flac": "flac",
    "amr": "amr", "m4a": "m4a", "x-m4a": "m4a", "mp4": "m4a", "webm": "webm",
    "pcm": "pcm", "raw": "raw",
}

# 按扩展名定 MIME。上传的文件要带正确的类型，文件服务器才认得出来
IMAGE_MIME = {
    "png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg",
    "gif": "image/gif", "webp": "image/webp", "bmp": "image/bmp",
}
AUDIO_MIME = {
    "wav": "audio/wav", "mp3": "audio/mpeg", "aac": "audio/aac",
    "ogg": "audio/ogg", "opus": "audio/ogg", "flac": "audio/flac",
    "amr": "audio/amr", "m4a": "audio/mp4", "pcm": "audio/pcm",
}

class FileServer:
    """把工具回包里的图片和音频存到文件服务器。

    上传走 upload_url，下载地址用 public_url。文件服务器在内网、模型在云上
    时，模型只能抓公网地址，两个地址要分开配。
    """

    def __init__(self, upload_url, public_url=None, timeout=30.0):
        self.upload_url = upload_url.rstrip("/")
        self.public_url = (public_url or upload_url).rstrip("/")
        self.timeout = timeout
        # 文件服务器是本机服务，走系统代理会把上传发给代理，代理不认这个接口
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def upload(self, data, filename, mime):
        """上传一段二进制，返回下载地址；失败返回 None。"""
        boundary = "----agentmcp{}".format(uuid.uuid4().hex)
        head = (
            "--{b}\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"{n}\"\r\n"
            "Content-Type: {m}\r\n\r\n"
        ).format(b=boundary, n=filename, m=mime)
        tail = "\r\n--{}--\r\n".format(boundary)
        body = head.encode("utf-8") + data + tail.encode("utf-8")
        request = urllib.request.Request(
            "{}/upload".format(self.upload_url), data=body, method="POST",
            headers={
                "Content-Type": "multipart/form-data; boundary={}".format(boundary),
                "Content-Length": str(len(body)),
            })
        try:
            with self._opener.open(request, timeout=self.timeout) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except Exception as exc:
            print("[mcp] 文件存到文件服务器失败：{}".format(exc))
            return None
        file_id = ((payload or {}).get("data") or {}).get("file_id")
        if not file_id:
            print("[mcp] 文件服务器没回 file_id")
            return None
        return "{}/download/{}".format(self.public_url, file_id)


def media_suffix(mime):
    """按 MIME 的子类型定扩展名。"""
    subtype = (mime or "").split("/")[-1].lower()
    return MEDIA_SUFFIX.get(subtype, "bin")


def media_name(base, mime):
    """给上传的文件起个名字：工具名加上按 MIME 定的扩展名。"""
    safe = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in (base or ""))
    if not safe:
        safe = "media"
    return "{}.{}".format(safe, media_suffix(mime))


def mime_of(path, kind="image"):
    """按扩展名判断文件的 MIME，判断不出来就用这一类的默认类型。"""
    ext = os.path.splitext(path)[1].lstrip(".").lower()
    table = AUDIO_MIME if kind == "audio" else IMAGE_MIME
    if ext in table:
        return table[ext]
    guessed = mimetypes.guess_type(path)[0]
    if guessed:
        return guessed
    return DEFAULT_MIME.get(kind, "application/octet-stream")


async def store_media(kind, source):
    """把图片或音频存到文件服务器，返回下载地址。

    source 可以是本地文件路径、原始字节，或者 http 地址。http 地址直接拿去用，
    不上传。没配置文件服务器时抛错，工具按失败返回。

    文件服务器和工具名从当前上下文里取，上传走网络，放到线程里跑，不占着
    事件循环。
    """
    if isinstance(source, str) and source.startswith(("http://", "https://")):
        return source
    context = current_context()
    client = context.file_server if context else None
    if client is None:
        raise ValueError("没有配置文件服务器，{} 存不了".format(kind))
    if isinstance(source, (bytes, bytearray)):
        data = bytes(source)
        mime = DEFAULT_MIME.get(kind, "application/octet-stream")
        base = (context.tool_name if context else "") or kind
    else:
        path = os.path.abspath(os.path.expanduser(str(source)))
        if not os.path.isfile(path):
            raise ValueError("{} 文件不存在：{}".format(kind, source))
        data = await asyncio.to_thread(_read_file, path)
        mime = mime_of(path, kind)
        base = os.path.splitext(os.path.basename(path))[0]
    url = await asyncio.to_thread(client.upload, data, media_name(base, mime), mime)
    if not url:
        raise ValueError("{} 存到文件服务器失败".format(kind))
    return url


def _read_file(path):
    """读一个文件的内容。"""
    with open(path, "rb") as handle:
        return handle.read()
