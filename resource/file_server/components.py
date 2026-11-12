"""文件服务器的组件清单与依赖探测。

组件有四个：file-server（核心）、mcp（网关）、tts（语音合成）、stt（语音转写）。
部署脚本按组件启停、按组件装依赖，这里只回答一件事：某个组件的依赖齐了没有。

用法：
    python components.py status          # 人看的清单
    python components.py status --json   # 机器读的清单

依赖探测只查文件与已装的包，不下载任何东西。
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# 中文 Windows 的 stdout 默认 GBK。控制台读这个脚本的输出时按 UTF-8 解，
# 这里必须显式改，否则组件名与说明全是乱码。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

BASE_DIR = Path(__file__).resolve().parent
TTS_ROOT = BASE_DIR / "third_party" / "gpt-sovits-infer"
MCP_DIR = BASE_DIR / "mcp"

# 组件键与显示名。顺序就是页面与命令行里的展示顺序。
COMPONENTS = ("file-server", "mcp", "tts", "stt")
NAMES = {
    "file-server": "文件服务器",
    "mcp": "MCP 网关",
    "tts": "语音合成",
    "stt": "语音转写",
}
# 文件服务器本体是其它组件的地基，不能关。
REQUIRED_COMPONENTS = ("file-server",)

# whisper 的模型文件名。openai-whisper 按名字取文件，名字到文件名的映射写死在这里，
# 免得为了拿这张表去 import whisper（那会把 torch 一起拖进来，几秒起步）。
WHISPER_FILES = {
    "tiny": "tiny.pt",
    "base": "base.pt",
    "small": "small.pt",
    "medium": "medium.pt",
    "large": "large.pt",
    "large-v1": "large-v1.pt",
    "large-v2": "large-v2.pt",
    "large-v3": "large-v3.pt",
}

# 核心组件要用的包。版本取不到就算没装。
CORE_PACKAGES = ("fastapi", "uvicorn", "pydantic", "starlette")
TTS_PACKAGES = ("torch", "librosa", "transformers", "modelscope")
STT_PACKAGES = ("openai-whisper", "soundfile", "resampy")

# GPT-SoVITS 的预训练模型。缺一个推理就起不来。
TTS_PRETRAINED = {
    "chinese-hubert-base": "GPT_SoVITS/pretrained_models/chinese-hubert-base/pytorch_model.bin",
    "chinese-roberta-wwm-ext-large": "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/pytorch_model.bin",
    "G2PWModel": "GPT_SoVITS/text/G2PWModel/g2pW.onnx",
    "fasttext LID": "GPT_SoVITS/pretrained_models/fast_langdetect/lid.176.bin",
}
# 权重目录。目录名就是音色的推理版本。
TTS_WEIGHT_DIRS = {
    "v2": ("GPT_weights_v2", "SoVITS_weights_v2"),
    "v2Pro": ("GPT_weights_v2Pro", "SoVITS_weights_v2Pro"),
    "v2ProPlus": ("GPT_weights_v2ProPlus", "SoVITS_weights_v2ProPlus"),
}


def venv_python():
    """venv 里的 python。Windows 在 Scripts/ 下，其余在 bin/ 下。"""
    for rel in (Path("Scripts/python.exe"), Path("bin/python")):
        path = BASE_DIR / ".venv" / rel
        if path.is_file():
            return path
    return None


# 探测结果的缓存。torch 的 import 要几十秒，运维页几秒刷一次状态，
# 没有缓存的话每次都重跑一遍探测。装完依赖后部署脚本会删掉这个文件。
CACHE_FILE = BASE_DIR / ".components_cache.json"
CACHE_TTL = 3600.0


def _cache_read():
    try:
        return json.loads(CACHE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _cache_write(data):
    try:
        CACHE_FILE.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def _cache_get(key):
    entry = _cache_read().get(key)
    if not isinstance(entry, dict):
        return None
    if time.time() - float(entry.get("at", 0)) > CACHE_TTL:
        return None
    return entry.get("value")


def _cache_put(key, value):
    data = _cache_read()
    data[key] = {"at": time.time(), "value": value}
    _cache_write(data)


def _run(python, code, args=(), timeout=90):
    """跑一小段 python 拿输出。失败返回 None，不抛异常。"""
    try:
        proc = subprocess.run(
            [str(python), "-c", code, *[str(a) for a in args]],
            capture_output=True,
            timeout=timeout,
            cwd=str(BASE_DIR),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", "replace")


_VERSION_CODE = (
    "import importlib.metadata as m,sys\n"
    "for n in sys.argv[1:]:\n"
    "    try:\n"
    "        print(n+'='+m.version(n))\n"
    "    except Exception:\n"
    "        print(n+'=')\n"
)

_TORCH_CODE = (
    "import json,torch\n"
    "print(json.dumps({'version': torch.__version__,"
    " 'cuda': bool(torch.cuda.is_available())}))\n"
)


def package_versions(python, names):
    """一次子进程取一批包的版本，返回 {name: version}。没装的版本是空串。"""
    if not python or not names:
        return {}
    out = _run(python, _VERSION_CODE, names)
    versions = {}
    for line in (out or "").splitlines():
        if "=" not in line:
            continue
        name, _, version = line.partition("=")
        versions[name.strip()] = version.strip()
    for name in names:
        versions.setdefault(name, "")
    return versions


def torch_info(python):
    """torch 的版本与 CUDA 可用性。import torch 要几秒到几十秒，失败返回 None。

    结果进缓存：探测一次几十秒，运维页每几秒就要刷一次状态。
    """
    cached = _cache_get("torch")
    if cached is not None:
        return cached or None
    out = _run(python, _TORCH_CODE, timeout=180)
    if out is None:
        _cache_put("torch", {})
        return None
    try:
        info = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        _cache_put("torch", {})
        return None
    _cache_put("torch", info)
    return info


def whisper_cache_dir():
    """openai-whisper 的模型缓存目录。"""
    return Path(os.path.expanduser("~")) / ".cache" / "whisper"


def whisper_model_file():
    """当前 STT_MODEL 对应的模型文件。"""
    model = os.getenv("STT_MODEL", "tiny").strip() or "tiny"
    return whisper_cache_dir() / WHISPER_FILES.get(model, model + ".pt")


def tts_voices():
    """能用的音色。权重成对、参考音频与配置都在才算一个。"""
    voices = []
    for version, (gpt_dir, sovits_dir) in TTS_WEIGHT_DIRS.items():
        gpts = sorted((TTS_ROOT / gpt_dir).glob("*.ckpt"))
        sovits = sorted((TTS_ROOT / sovits_dir).glob("*.pth"))
        for gpt in gpts:
            stem = gpt.stem.split("-e")[0]
            match = None
            for pth in sovits:
                if pth.stem.split("_e")[0] == stem:
                    match = pth
                    break
            if match is None:
                continue
            cfg = TTS_ROOT / "tts_speakers" / (stem + ".json")
            if not cfg.is_file():
                continue
            try:
                ref = json.loads(cfg.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            audio = TTS_ROOT / str(ref.get("ref_audio", ""))
            text = TTS_ROOT / str(ref.get("ref_text", ""))
            if not audio.is_file() or not text.is_file():
                continue
            voices.append({"name": stem, "version": version})
    return voices


def _dep(name, ok, detail, required=True):
    return {"name": name, "ok": bool(ok), "detail": detail, "required": required}


def probe_file_server(python, versions):
    deps = []
    if python:
        deps.append(_dep("虚拟环境", True, str(python.relative_to(BASE_DIR))))
    else:
        deps.append(_dep("虚拟环境", False, "还没有 .venv，跑一次部署脚本"))

    missing = [n for n, v in versions.items() if not v]
    if missing:
        deps.append(_dep("Python 包", False, "缺 " + "、".join(missing)))
    else:
        deps.append(_dep("Python 包", True,
                         "、".join("%s %s" % (n, versions[n]) for n in CORE_PACKAGES)))
    deps.append(_dep("文件数据库", (BASE_DIR / "file_db.sqlite").is_file(),
                     "file_db.sqlite", required=False))
    return deps


def probe_mcp(python, versions):
    deps = [_dep("网关脚本", (MCP_DIR / "mcp_gateway.py").is_file(),
                 "mcp/mcp_gateway.py")]
    deps.append(_dep("网关代码包", (MCP_DIR / "gateway").is_dir(),
                     "mcp/gateway", required=False))

    node = shutil.which("node")
    deps.append(_dep("Node.js", bool(node), node or "没装，桌面工具用不了", required=False))

    mcp_exe = shutil.which("windows-mcp")
    if not mcp_exe:
        tool_dir = os.path.expanduser("~/.local/bin")
        candidate = Path(tool_dir) / ("windows-mcp.exe" if os.name == "nt" else "windows-mcp")
        mcp_exe = str(candidate) if candidate.is_file() else None
    deps.append(_dep("Windows-MCP", bool(mcp_exe),
                     mcp_exe or "没装，网关只有内置的文件工具", required=False))
    return deps


def probe_tts(python, versions, torch):
    deps = []
    deps.append(_dep("GPT-SoVITS 推理代码",
                     (TTS_ROOT / "GPT_SoVITS/TTS_infer_pack/TTS.py").is_file(),
                     "third_party/gpt-sovits-infer"))

    missing = [n for n, v in versions.items() if not v and n != "torch"]
    if torch:
        detail = "torch %s" % torch["version"]
        if torch.get("cuda"):
            detail += "，CUDA 可用"
        else:
            detail += "，只有 CPU"
        deps.append(_dep("torch", True, detail))
    else:
        deps.append(_dep("torch", False, "没装或 import 失败"))
    if missing:
        deps.append(_dep("TTS 的 Python 包", False, "缺 " + "、".join(missing)))
    else:
        deps.append(_dep("TTS 的 Python 包", True,
                         "、".join("%s %s" % (n, versions[n]) for n in TTS_PACKAGES if n != "torch")))

    absent = [name for name, rel in TTS_PRETRAINED.items() if not (TTS_ROOT / rel).is_file()]
    if absent:
        deps.append(_dep("预训练模型", False, "缺 " + "、".join(absent)))
    else:
        deps.append(_dep("预训练模型", True, "%d 个齐全" % len(TTS_PRETRAINED)))

    voices = tts_voices()
    if voices:
        deps.append(_dep("音色权重", True,
                         "、".join("%s(%s)" % (v["name"], v["version"]) for v in voices)))
    else:
        deps.append(_dep("音色权重", False, "没有成套的 .ckpt + .pth + 参考音频"))

    runtime = TTS_ROOT / "runtime"
    ffmpeg = runtime / ("avcodec-63.dll" if os.name == "nt" else "")
    if os.name != "nt":
        ok = runtime.is_dir()
        detail = str(runtime.relative_to(BASE_DIR))
    else:
        ok = ffmpeg.is_file()
        detail = str(ffmpeg.relative_to(BASE_DIR))
    deps.append(_dep("FFmpeg 运行库", ok, detail))
    return deps


def probe_stt(python, versions):
    deps = []
    missing = [n for n, v in versions.items() if not v]
    if missing:
        deps.append(_dep("Python 包", False, "缺 " + "、".join(missing)))
    else:
        deps.append(_dep("Python 包", True,
                         "、".join("%s %s" % (n, versions[n]) for n in STT_PACKAGES)))

    model = whisper_model_file()
    if model.is_file():
        deps.append(_dep("whisper 模型", True, str(model)))
    else:
        deps.append(_dep("whisper 模型", False,
                         "%s 不在，第一次转写时会现下" % model.name))
    return deps


def probe_all():
    """探测全部组件。返回可以直接 JSON 化的字典。"""
    python = venv_python()
    probe_python = python or Path(sys.executable)

    # 一次子进程问完所有包版本。分开问要多起几次进程。
    wanted = tuple(dict.fromkeys(CORE_PACKAGES + TTS_PACKAGES + STT_PACKAGES))
    found = package_versions(probe_python, wanted)
    core = {n: found.get(n, "") for n in CORE_PACKAGES}
    tts_pkgs = {n: found.get(n, "") for n in TTS_PACKAGES if n != "torch"}
    stt_pkgs = {n: found.get(n, "") for n in STT_PACKAGES}
    torch = torch_info(probe_python)

    tts_versions = dict(tts_pkgs)
    tts_versions["torch"] = torch["version"] if torch else ""
    probes = {
        "file-server": lambda: probe_file_server(python, core),
        "mcp": lambda: probe_mcp(python, core),
        "tts": lambda: probe_tts(python, tts_versions, torch),
        "stt": lambda: probe_stt(python, stt_pkgs),
    }

    components = []
    for key in COMPONENTS:
        deps = probes[key]()
        required = [d for d in deps if d["required"]]
        components.append({
            "key": key,
            "name": NAMES[key],
            "required": key in REQUIRED_COMPONENTS,
            "ready": all(d["ok"] for d in required),
            "deps": deps,
        })

    return {
        "generatedAt": time.time(),
        "python": str(probe_python),
        "components": components,
    }


def print_human(report):
    """给人看的清单。"""
    for comp in report["components"]:
        flag = "就绪" if comp["ready"] else "缺依赖"
        print("[%s] %s：%s" % (comp["key"], comp["name"], flag))
        for dep in comp["deps"]:
            mark = "ok   " if dep["ok"] else "缺   "
            if not dep["required"]:
                mark = "可选 " if dep["ok"] else "可选 "
            print("    %s %-22s %s" % (mark, dep["name"], dep["detail"]))
    print()
    print("探测用的 python：" + report["python"])


def main(argv):
    args = [a for a in argv[1:] if a not in ("--json",)]
    if args and args[0] == "status":
        report = probe_all()
        if "--json" in argv:
            print(json.dumps(report, ensure_ascii=False))
        else:
            print_human(report)
        return 0
    print(__doc__.strip())
    print("\n用法：python components.py status [--json]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
