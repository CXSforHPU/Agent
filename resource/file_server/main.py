from fastapi import FastAPI, UploadFile, File, HTTPException, Response
from fastapi.responses import FileResponse
from pydantic import BaseModel
import os
import re
import sys
import json
import uuid
import sqlite3
import mimetypes
import threading
import asyncio
import traceback
from datetime import datetime
from io import BytesIO
from pathlib import Path
from typing import Dict, Any, Optional, Tuple

app = FastAPI(title="嵌入式流式文件服务", version="1.0")

# 上游 GPT-SoVITS 的 TextPreprocessor 里直接 print(text)。中文 Windows 的
# stdout 默认 GBK，回复带 emoji 时 print 抛 UnicodeEncodeError，/tts 变 500。
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# /tts 会把工作目录切到 third_party/gpt-sovits-infer（上游用相对路径找 G2PWModel 与
# nltk_data），所以本服务自己的路径一律取绝对值。
BASE_DIR = Path(__file__).resolve().parent
UPLOAD_DIR = str(BASE_DIR / "upload_files")
DB_PATH = str(BASE_DIR / "file_db.sqlite")
os.makedirs(UPLOAD_DIR, exist_ok=True)


def store_path_of(stored: str) -> str:
    return os.path.join(BASE_DIR, stored)


def init_db():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("""
    CREATE TABLE IF NOT EXISTS file_record (
        file_id TEXT PRIMARY KEY,
        original_name TEXT,
        store_path TEXT,
        file_size INTEGER,
        upload_time TEXT
    )
    """)
    conn.commit()
    conn.close()


init_db()


def dict_from_row(row, cur) -> Dict[str, Any]:
    if row is None:
        return None
    return {desc[0]: row[i] for i, desc in enumerate(cur.description)}


@app.post("/upload", summary="嵌入式multipart/form‑data流式上传，form字段name=file")
async def upload(file: UploadFile = File(...)):
    """
    对接RT‑Thread webclient流式multipart上传
    form‑data: name="file"; filename="xxx.jpg"
    返回 {"code":0, "data":{"file_id":"xxx",...}}
    """
    file_id = str(uuid.uuid4())
    suffix = os.path.splitext(file.filename)[1]
    store_filename = f"{file_id}{suffix}"
    store_path = os.path.join(UPLOAD_DIR, store_filename)

    # 流式写入（推荐）
    with open(store_path, "wb") as f:
        while True:
            chunk = await file.read(1024 * 1024)  # 每次 1MB
            if not chunk:
                break
            f.write(chunk)

    file_size = os.path.getsize(store_path)
    upload_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # ... 数据库记录等后续代码保持不变 ...

    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("""
        INSERT INTO file_record(file_id, original_name, store_path, file_size, upload_time)
        VALUES (?,?,?,?,?)
    """, (file_id, file.filename, store_path, file_size, upload_time))
    conn.commit()
    conn.close()

    return {
        "code": 0,
        "msg": "upload ok",
        "data": {
            "file_id": file_id,
            "original_name": file.filename,
            "store_path": store_path,
            "file_size": file_size,
            "upload_time": upload_time
        }
    }


def guess_media_type(path: str) -> str:
    """按文件魔数嗅探优先、扩展名兜底推断 Content-Type（不依赖第三方库）。

    背景：LLM 多模态服务端按 Content-Type 判断资源类型。
    - .amr（嵌入式最常用语音格式）Python mimetypes 不认识，会退化为 application/octet-stream；
    - .raw/.pcm 裸音频会被 mimetypes 误判为 image/RAW；
    均会导致模型按错误容器解码音频而失真，故优先用魔数识别。
    """
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError:
        return "application/octet-stream"

    # ---- 图片魔数 ----
    if head[:3] == b"\xff\xd8\xff":
        return "image/jpeg"
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "image/png"
    if head[:6] in (b"GIF87a", b"GIF89a"):
        return "image/gif"
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return "image/webp"
    if head[:2] == b"BM":
        return "image/bmp"

    # ---- 音频魔数 ----
    if head[:4] == b"RIFF" and head[8:12] == b"WAVE":
        return "audio/wav"
    if head[0] == 0xFF and (head[1] & 0xF6) == 0xF0:
        return "audio/aac"           # AAC ADTS 帧同步：0xFF F0/F1/F8/F9
    if head[:3] == b"ID3" or (head[0] == 0xFF and (head[1] & 0xE0) == 0xE0):
        return "audio/mpeg"          # MP3：ID3 标签或帧同步 0xFF Ex/Fx
    if head[:5] == b"#!AMR":
        return "audio/amr"           # AMR-NB/WB（嵌入式最常用）
    if head[:4] == b"OggS":
        return "audio/ogg"           # OGG / Opus
    if head[:4] == b"fLaC":
        return "audio/flac"

    # ---- 扩展名兜底（排除 mimetypes 对 .raw 的误判） ----
    mt, _ = mimetypes.guess_type(path)
    if mt and mt != "image/RAW":
        return mt
    return "application/octet-stream"


@app.get("/download/{file_id}", summary="根据file_id下载文件，供嵌入式GET下载")
def download_file(file_id: str):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("SELECT * FROM file_record WHERE file_id = ?", (file_id,))
    row = cur.fetchone()
    conn.close()
    item = dict_from_row(row, cur)
    if not item:
        raise HTTPException(status_code=404, detail="file_id not found")
    path = store_path_of(item["store_path"])
    if not os.path.exists(path):
        raise HTTPException(status_code=404, detail="file lost on disk")

    return FileResponse(
        path=path,
        filename=item["original_name"],
        media_type=guess_media_type(path),
        content_disposition_type="inline",
    )


@app.get("/files/{file_id}", summary="查询单文件元信息")
def get_file_info(file_id: str):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("SELECT * FROM file_record WHERE file_id = ?", (file_id,))
    row = cur.fetchone()
    conn.close()
    item = dict_from_row(row, cur)
    if not item:
        raise HTTPException(status_code=404, detail="file_id not found")
    return {"code": 0, "data": item}


@app.get("/files", summary="获取全部文件列表")
def list_all_files():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("SELECT * FROM file_record ORDER BY upload_time DESC;")
    rows = cur.fetchall()
    data = [dict_from_row(r, cur) for r in rows]
    conn.close()
    return {"code": 0, "data": data}


@app.delete("/files/{file_id}", summary="删除文件")
def delete_file(file_id: str):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("SELECT * FROM file_record WHERE file_id = ?", (file_id,))
    row = cur.fetchone()
    item = dict_from_row(row, cur)
    if not item:
        conn.close()
        raise HTTPException(status_code=404, detail="file_id not found")

    path = store_path_of(item["store_path"])
    if os.path.exists(path):
        os.remove(path)
    cur.execute("DELETE FROM file_record WHERE file_id=?", (file_id,))
    conn.commit()
    conn.close()
    return {"code": 0, "msg": "deleted"}


# ---------------------------------------------------------------------------
# 本地语音转文本 (轻量化，openai-whisper tiny/base)
# 接受与 /upload 同样的 multipart file 字段，返回 {"code":0,"data":{"text":...}}
# ---------------------------------------------------------------------------
_STT_MODEL = os.getenv("STT_MODEL", "tiny")        # tiny / base / small
_STT_LANG  = os.getenv("STT_LANG",  "zh")
_stt_lock  = threading.Lock()
_stt_model = None


def _stt_get_model():
    global _stt_model
    with _stt_lock:
        if _stt_model is None:
            import whisper
            print(f"[stt] loading whisper model: {_STT_MODEL} (lang={_STT_LANG}) ...",
                  flush=True)
            _stt_model = whisper.load_model(_STT_MODEL)
            print("[stt] model ready", flush=True)
        return _stt_model


def _stt_transcribe(tmp_path):
    import soundfile as sf

    model = _stt_get_model()
    # whisper 需要 16 kHz 单声道 float32 in [-1, 1]；soundfile 直读避免 ffmpeg 依赖
    audio, sr = sf.read(tmp_path, dtype="float32")
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if sr != 16000:
        # 传 ndarray 时 whisper 不重采样（只有传路径才走 load_audio + ffmpeg），
        # 44.1 kHz 会被当成 16 kHz 播放，识别退化成乱码。
        import resampy
        audio = resampy.resample(audio, sr, 16000)
    with _stt_lock:
        result = model.transcribe(audio, language=_STT_LANG,
                                  fp16=False, beam_size=1)
    return (result.get("text") or "").strip()


@app.on_event("startup")
def _stt_preload():
    """启动时后台加载模型，避免板子第一次 /stt 请求等模型下载/加载数十秒。"""
    def _work():
        try:
            _stt_get_model()
        except Exception as e:
            print(f"[stt] preload failed: {e}", flush=True)

    threading.Thread(target=_work, daemon=True).start()


@app.post("/stt", summary="本地语音转文本")
async def stt(file: UploadFile = File(...)):
    tmp_path = os.path.join(UPLOAD_DIR, f"stt_{uuid.uuid4().hex}.wav")
    body = await file.read()
    with open(tmp_path, "wb") as f:
        f.write(body)
    try:
        # 识别是 CPU 密集的同步调用，放到线程池里跑，避免阻塞事件循环导致
        # 同一进程的 /upload、/download 请求排队（板子会表现为连接超时）。
        text = await asyncio.to_thread(_stt_transcribe, tmp_path)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"stt failed: {e}")
    finally:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
    return {"code": 0, "data": {"text": text}}


# ---------------------------------------------------------------------------
# 语音合成 (GPT-SoVITS v2 / v2Pro)。推理代码在 third_party/gpt-sovits-infer，模型与权重靠
# fetch_tts.py 取；没装配时 /tts 返回 503，其余接口不受影响。
_TTS_ROOT = BASE_DIR / "third_party" / "gpt-sovits-infer"
_TTS_SPEAKERS_DIR = _TTS_ROOT / "tts_speakers"
# 权重按训练版本分目录，目录名就是这个音色的推理版本。换权重时上游会自己读 ckpt 头
# 判版本（process_ckpt.py:77 head2version），Pro 系列还会顺手加载 ERes2NetV2 说话人
# 模型（TTS.py:584 init_sv_model），所以一条管线可以跨版本切。本仓库自带的是
# v2Pro 音色，v2 那两个目录留空，放第三方 v2 权重进去也能用。
_TTS_WEIGHT_DIRS = {
    "v2": ("GPT_weights_v2", "SoVITS_weights_v2"),
    "v2Pro": ("GPT_weights_v2Pro", "SoVITS_weights_v2Pro"),
}
_TTS_LOCK = threading.Lock()
_TTS_PIPELINE = None
_TTS_WEIGHTS: Optional[Tuple[str, str]] = None
# TTS_infer_pack 只认引擎语种码（TTS.py:1251 assert text_lang in configs.languages），
# WebUI 那套中文名靠这张表换算，两种写法都接受。
_TTS_LANGS = ("all_zh", "all_ja", "all_yue", "all_ko", "zh", "ja", "yue", "ko", "en", "auto", "auto_yue")
_TTS_LANG_ALIAS = {
    "中文": "all_zh", "日文": "all_ja", "粤语": "all_yue", "韩文": "all_ko", "英文": "en",
    "中英混合": "zh", "日英混合": "ja", "粤英混合": "yue", "韩英混合": "ko",
    "多语种混合": "auto", "多语种混合(粤语)": "auto_yue",
}
_TTS_UNAVAILABLE = ("TTS 未装配：先跑 setup_env.sh --tts（或 .\\setup_env.ps1 -Tts）"
                    "与 python fetch_tts.py dicts voice assets")


def _tts_lang(value: str) -> str:
    """中文名或引擎码 -> 引擎码；不在表内原样返回，由调用处判错。"""
    return _TTS_LANG_ALIAS.get(value, value)


def _tts_stem(path: Path, pattern: str) -> str:
    return re.split(pattern, path.stem, maxsplit=1)[0]


def _tts_speakers() -> Dict[str, Dict[str, str]]:
    """权重目录里的 .ckpt 与 .pth 按权重名前缀配成音色，参考音频与文本读
    tts_speakers/<name>.json，缺 JSON 的音色不列入。"""
    weights: Dict[str, Dict[str, str]] = {}
    for version, (gpt_dir, sovits_dir) in _TTS_WEIGHT_DIRS.items():
        for p in sorted((_TTS_ROOT / gpt_dir).glob("*.ckpt")):
            w = weights.setdefault(_tts_stem(p, r"-e\d+"), {})
            w["gpt"] = str(p)
            w["version"] = version
        for p in sorted((_TTS_ROOT / sovits_dir).glob("*.pth")):
            w = weights.setdefault(_tts_stem(p, r"_e\d+_s\d+"), {})
            w["sovits"] = str(p)
            w.setdefault("version", version)

    speakers: Dict[str, Dict[str, str]] = {}
    for name, w in weights.items():
        if "gpt" not in w or "sovits" not in w:
            continue
        cfg = _TTS_SPEAKERS_DIR / f"{name}.json"
        if not cfg.is_file():
            print(f"[tts] {name}: 缺 {cfg}，跳过", flush=True)
            continue
        ref = json.loads(cfg.read_text(encoding="utf-8"))
        if not ref.get("ref_audio") or not ref.get("ref_text"):
            print(f"[tts] {name}: {cfg} 缺 ref_audio / ref_text，跳过", flush=True)
            continue
        ref_text = Path(os.path.join(_TTS_ROOT, ref["ref_text"]))
        if not ref_text.is_file():
            print(f"[tts] {name}: 参考文本 {ref_text} 不存在，跳过", flush=True)
            continue
        speakers[name] = {
            "gpt": w["gpt"],
            "sovits": w["sovits"],
            "version": w["version"],
            "ref_audio": os.path.join(_TTS_ROOT, ref["ref_audio"]),
            "prompt_text": ref_text.read_text(encoding="utf-8").strip(),
            "prompt_lang": _tts_lang(ref.get("ref_language", "中文")),
        }
    return speakers


def _tts_default(speakers: Dict[str, Dict[str, str]]) -> str:
    return os.getenv("TTS_SPEAKER") or sorted(speakers)[0]


def _tts_activate():
    os.chdir(_TTS_ROOT)
    sys.path.insert(0, str(_TTS_ROOT))
    from tools.portable_runtime import activate
    activate()


def _tts_pipeline(sp: Dict[str, str]):
    """首帧建 TTS，之后换音色只重载两套权重；调用方全程持 _TTS_LOCK。"""
    global _TTS_PIPELINE, _TTS_WEIGHTS
    import torch

    target = (sp["gpt"], sp["sovits"])
    if _TTS_PIPELINE is None:
        _tts_activate()
        from GPT_SoVITS.TTS_infer_pack.TTS import TTS, TTS_Config

        device = os.getenv("TTS_DEVICE") or ("cuda" if torch.cuda.is_available() else "cpu")
        is_half = (os.getenv("TTS_IS_HALF") or ("1" if device == "cuda" else "0")) == "1"
        _TTS_PIPELINE = TTS(TTS_Config({"custom": {
            "device": device,
            "is_half": is_half,
            "version": sp["version"],
            "t2s_weights_path": sp["gpt"],
            "vits_weights_path": sp["sovits"],
            "cnhuhbert_base_path": "GPT_SoVITS/pretrained_models/chinese-hubert-base",
            "bert_base_path": "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large",
        }}))
        _TTS_WEIGHTS = target
        print(f"[tts] pipeline ready on {device} (is_half={is_half})", flush=True)
    elif _TTS_WEIGHTS != target:
        _TTS_PIPELINE.init_t2s_weights(sp["gpt"])
        _TTS_PIPELINE.init_vits_weights(sp["sovits"])
        _TTS_WEIGHTS = target
        print("[tts] weights switched to " + sp["gpt"], flush=True)
    return _TTS_PIPELINE


def _tts_synth_wav(sp: Dict[str, str], text: str, language: str, speed: float,
                   out_sr: Optional[int], channels: int = 1) -> Tuple[int, bytes]:
    import numpy as np
    import soundfile as sf

    with _TTS_LOCK:
        pipe = _tts_pipeline(sp)
        fragments = list(pipe.run({
            "text": text,
            "text_lang": language,
            "ref_audio_path": sp["ref_audio"],
            "prompt_text": sp["prompt_text"],
            "prompt_lang": sp["prompt_lang"],
            "text_split_method": "cut5",
            "batch_size": 1,
            "speed_factor": speed,
        }))
    if not fragments:
        raise RuntimeError("TTS 未产出音频，检查文本与参考音频")

    sr = fragments[0][0]
    # TTS.run() 交的是 int16 PCM（TTS.py:1814、:1822 乘 32768 后 astype(int16)），
    # 必须先归一到 [-1,1] 再重采样，否则 resampy 拿到的是上万的幅值，写回 PCM_16 全被削顶。
    audio = np.concatenate([np.asarray(f[1], dtype=np.float32) / 32768.0 for f in fragments])
    if out_sr and out_sr != sr:
        import resampy
        audio = resampy.resample(audio, sr, out_sr)
        sr = out_sr
    if channels == 2:
        # AC97 帧固定带左右两个 PCM 槽，只接 AC97 的板子（如 vexpress 的 AACI）
        # 拿到单声道数据会把相邻两个采样当成一帧，播放速度和音调都翻倍。
        audio = np.repeat(audio[:, None], 2, axis=1)

    buf = BytesIO()
    sf.write(buf, audio, sr, format="WAV", subtype="PCM_16")
    return sr, buf.getvalue()


class TTSRequest(BaseModel):
    text: str
    speaker: Optional[str] = None
    language: str = "zh"
    speed: float = 1.0
    sample_rate: Optional[int] = None
    channels: int = 1


@app.get("/tts/speakers", summary="列出可用音色")
def tts_speakers():
    speakers = _tts_speakers()
    if not speakers:
        raise HTTPException(status_code=503, detail=_TTS_UNAVAILABLE)
    return {
        "code": 0,
        "data": {
            "speakers": sorted(speakers),
            "default": _tts_default(speakers),
            "languages": list(_TTS_LANGS),
            "language_alias": _TTS_LANG_ALIAS,
        },
    }


@app.post("/tts", summary="文本转语音，返回 audio/wav 字节")
async def tts(req: TTSRequest):
    speakers = _tts_speakers()
    if not speakers:
        raise HTTPException(status_code=503, detail=_TTS_UNAVAILABLE)
    text = req.text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="text 不能为空")
    # 表情符号念不出来，切句后还白占一段推理
    text = re.sub(r"[\U00010000-\U0010FFFF]", "", text).strip()
    if not text:
        raise HTTPException(status_code=400, detail="text 去掉表情符号后为空")
    language = _tts_lang(req.language)
    if language not in _TTS_LANGS:
        raise HTTPException(status_code=400,
                            detail="language 只能是 " + "/".join(_TTS_LANGS)
                                   + "，或对应中文名 " + "/".join(_TTS_LANG_ALIAS))
    name = req.speaker or _tts_default(speakers)
    sp = speakers.get(name)
    if sp is None:
        raise HTTPException(status_code=404,
                            detail=f"音色 {name} 不存在，可用: {', '.join(sorted(speakers))}")
    if req.channels not in (1, 2):
        raise HTTPException(status_code=400, detail="channels 只能是 1 或 2")
    try:
        # 推理是 GPU/CPU 密集同步调用，放线程池避免拖慢同进程的上传下载
        sr, wav = await asyncio.to_thread(_tts_synth_wav, sp, text,
                                         language, req.speed, req.sample_rate,
                                         req.channels)
    except HTTPException:
        raise
    except Exception as e:
        traceback.print_exc()
        raise HTTPException(status_code=500,
                            detail=f"tts failed: {type(e).__name__}: {e}")
    return Response(content=wav, media_type="audio/wav",
                    headers={"X-Sample-Rate": str(sr), "X-Speaker": name})


@app.on_event("startup")
def _tts_preload():
    """TTS_AUTOLOAD=1 时启动就预热模型，否则首帧请求要等 30 s 上下。"""
    if os.getenv("TTS_AUTOLOAD", "0") != "1":
        return
    speakers = _tts_speakers()
    if not speakers:
        print("[tts] TTS_AUTOLOAD=1 但没有可用音色，跳过预加载", flush=True)
        return

    def _work():
        try:
            with _TTS_LOCK:
                _tts_pipeline(speakers[_tts_default(speakers)])
        except Exception as e:
            print(f"[tts] preload failed: {e}", flush=True)

    threading.Thread(target=_work, daemon=True).start()


if __name__ == "__main__":
    import uvicorn
    # 限制最大请求50MB，根据你的场景修改
    uvicorn.run(
        app,
        host=os.getenv("HOST", "0.0.0.0"),
        port=int(os.getenv("PORT", "8000")),
    )