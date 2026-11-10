"""文件工具共用的助手。

所有路径都限制在根目录内，越界一律拒绝。
"""
import asyncio
import datetime
import json
import os

TEXT_SUFFIXES = {".txt", ".md", ".log", ".csv", ".json", ".xml", ".yml", ".yaml",
                 ".py", ".c", ".h", ".ini", ".cfg", ".conf", ".sh", ".bat"}


def string_prop(kind):
    """参数表里的一个字符串参数。"""
    return {"type": "string", "title": kind}


def timestamp(stamp):
    """时间戳转成 ISO 8601 字符串。"""
    if stamp is None:
        return None
    return datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).isoformat()


def resolve(root, path, must_exist=True):
    """把输入路径解析成根目录内的绝对路径，越界直接拒绝。"""
    if not path:
        raise ValueError("path is required")
    target = path if os.path.isabs(path) else os.path.join(root, path)
    target = os.path.realpath(target)
    if target != root and not target.startswith(root + os.sep):
        raise ValueError("path outside of the allowed root: {}".format(path))
    if must_exist and not os.path.exists(target):
        raise ValueError("path does not exist: {}".format(path))
    return target


def match(filename, patterns):
    """文件名里有没有匹配上的模式。"""
    if not patterns:
        return True
    name = filename.lower()
    return any(pattern.lower() in name for pattern in patterns)


def snippet(path):
    """文本文件的头 200 个字符，二进制文件返回空串。"""
    if os.path.splitext(path)[1].lower() not in TEXT_SUFFIXES:
        return ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read(200)
    except OSError:
        return ""


def dump(payload):
    """结果转成给模型看的 JSON 文本。"""
    return json.dumps(payload, ensure_ascii=False, indent=2)


async def run_tool(func, *args):
    """在线程里跑同步的工具逻辑，出错按失败返回。

    文件工具碰磁盘，逻辑都是同步的，丢到线程里跑不占事件循环。结果统一
    转成 JSON 文本给模型，出错时把原因写在文字里。
    """
    try:
        payload = await asyncio.to_thread(func, *args)
    except Exception as exc:
        return {"content": [{"type": "text", "text": "tool failed: {}".format(exc)}],
                "isError": True}
    return {"content": [{"type": "text", "text": dump(payload)}], "isError": False}
