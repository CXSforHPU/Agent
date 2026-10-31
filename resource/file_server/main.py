from fastapi import FastAPI, UploadFile, File, HTTPException, Response
from fastapi.responses import FileResponse, HTMLResponse, StreamingResponse
from pydantic import BaseModel
import os
import re
import sys
import json
import uuid
import struct
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
    # 与 v2Pro 共用 s1v3 底模和 SV 编码器；gpt_streaming 上游只放
    # v2ProPlus/v5turbo/v5dev（TTS_infer_pack/streaming.py:12），本服务实测没收益，
    # 见 _tts_stream_frames。
    "v2ProPlus": ("GPT_weights_v2ProPlus", "SoVITS_weights_v2ProPlus"),
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
_TTS_UNAVAILABLE = ("TTS 未装配：先跑 bash deploy.sh --tts（或 deploy.bat --tts）"
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
            stem = _tts_stem(p, r"-e\d+")
            w = weights.setdefault(stem, {})
            # 权重名是 v2Pro 与 v2ProPlus 唯一的区分处，同名会静默让后扫到的版本生效
            if w.get("version", version) != version:
                print(f"[tts] {stem}: {w['version']} 与 {version} 同名，生效 {version}，"
                      f"请给其中一份改名", flush=True)
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


# 排序首个是 da_fei_yv（v2Pro），部署机按听感把 v2ProPlus 那档定为默认。
# TTS_SPEAKER 优先于这里，换默认不用改代码。
_TTS_PREFERRED_SPEAKER = "da_fei_yv_plus"


def _tts_default(speakers: Dict[str, Dict[str, str]]) -> str:
    env = os.getenv("TTS_SPEAKER")
    if env:
        return env
    if _TTS_PREFERRED_SPEAKER in speakers:
        return _TTS_PREFERRED_SPEAKER
    return sorted(speakers)[0]


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

        # 引擎在 GPT_SoVITS/Accel/PyTorch/__init__.py:23 打开 cudnn.benchmark，每遇到
        # 一个新的卷积形状就现场搜一遍最优算法。流式声码器每句长度都换形状，同一条启动
        # 预热+首条真实回复的链路实测：开着时首帧 13.8 s，关掉后 1.7 s；代价是稳态
        # RTF 从 0.249 升到 0.263（约 6%），换掉几十秒的尖峰划算。
        torch.backends.cudnn.benchmark = False

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


# 板侧 wavplayer 的播放循环每次 fread(buf, 2048, 1, fp)，返回的是条目数：只要不满
# 一整条就当作文件结束，那半截一个样本都不会写进声卡（wavplayer.c:412）。1.5 s 一帧
# 数据段正好 48000 字节 = 23×2048 + 896，于是每个块边界丢 896 字节，也就是 28 ms 语音。
# 这里把数据段补零到 2048 的整数倍：28 ms 语音丢失换成 36 ms 静音垫尾，切口不再吃字。
_TTS_PLAY_ALIGN = 2048


def _tts_pad_frames(audio, channels: int):
    """把待写成 WAV 的采样补零到板侧一次 fread 的整数倍。"""
    import numpy as np

    align = _TTS_PLAY_ALIGN // (channels * 2)   # 一个采样帧占 channels*2 字节
    pad = (-audio.shape[0]) % align
    if not pad:
        return audio
    shape = (pad, channels) if audio.ndim == 2 else pad
    return np.concatenate([audio, np.zeros(shape, dtype=audio.dtype)])


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
    sf.write(buf, _tts_pad_frames(audio, channels), sr, format="WAV", subtype="PCM_16")
    return sr, buf.getvalue()


# 流式合成的一块要独立重采样时，resampy 会把块边界当成信号端点，滤波器过渡带被
# 削掉，听感是每块一声爆音。所以每次多带上一块尾部的若干输入样本、重采样后再丢掉
# 它们对应的输出（overlap-save）。32k→16k 是整数倍，丢掉的样本数正好是 ctx/2。
_TTS_RS_CTX = 512
# 帧头：4 字节小端 payload 长度，后面是一整个可直接播放的 WAV。板侧 webclient_read()
# 会在 HTTP chunk 边界提前返回，所以要靠这个长度自己拼帧。
_TTS_FRAME_HDR = 4


class _TtsBlocker:
    """把引擎逐块吐出的原生采样率 PCM 攒成 block_seconds 秒一块，每块自带完整 WAV 头。"""

    def __init__(self, sr_native: int, out_sr: Optional[int], channels: int, block_seconds: float):
        import numpy as np

        self.sr_native = sr_native
        self.out_sr = out_sr or sr_native
        self.channels = channels
        self.block = max(1, int(sr_native * block_seconds))
        self.buf = np.zeros(0, dtype=np.float32)
        self.tail = np.zeros(0, dtype=np.float32)

    def _emit(self, pcm) -> bytes:
        import numpy as np
        import soundfile as sf

        vin = pcm if self.tail.size == 0 else np.concatenate([self.tail, pcm])
        if self.out_sr != self.sr_native:
            import resampy
            audio = resampy.resample(vin, self.sr_native, self.out_sr)
        else:
            audio = vin
        drop = int(round(self.tail.size * self.out_sr / self.sr_native))
        audio = audio[drop:]
        if self.channels == 2:
            audio = np.repeat(audio[:, None], 2, axis=1)

        out = BytesIO()
        sf.write(out, _tts_pad_frames(audio, self.channels), self.out_sr,
                 format="WAV", subtype="PCM_16")
        self.tail = pcm[-_TTS_RS_CTX:]
        return struct.pack("<I", out.tell()) + out.getvalue()

    def push(self, chunk) -> list:
        import numpy as np

        # TTS.run() 交的是 int16，归一到 [-1,1] 才能进 resampy，否则会削顶
        self.buf = np.concatenate([self.buf, np.asarray(chunk, dtype=np.float32) / 32768.0])
        frames = []
        while self.buf.shape[0] >= self.block:
            piece, self.buf = self.buf[:self.block], self.buf[self.block:]
            frames.append(self._emit(piece))
        return frames

    def finish(self) -> list:
        if self.buf.shape[0] == 0:
            return []
        piece, self.buf = self.buf, self.buf[:0]
        return [self._emit(piece)]


def _tts_stream_frames(sp: Dict[str, str], text: str, language: str, speed: float,
                       out_sr: Optional[int], channels: int, block_seconds: float):
    """流式合成：边推边发，每帧 [u32 长度][WAV]。板侧第一块到手就能起播。"""
    # 锁要一直持到整条流发完：模型不是重入的，中途插进另一个请求会把这次推流打断，
    # 客户端断线时生成器被关闭，with 会正常释放锁
    with _TTS_LOCK:
        blocker = None
        sent = 0
        try:
            pipe = _tts_pipeline(sp)
            gen = pipe.run({
                "text": text,
                "text_lang": language,
                "ref_audio_path": sp["ref_audio"],
                "prompt_text": sp["prompt_text"],
                "prompt_lang": sp["prompt_lang"],
                "text_split_method": "cut5",
                "batch_size": 1,
                "speed_factor": speed,
                "streaming_mode": True,
                # 25 Hz 语义帧下 16 个 token = 0.64 s 音频，再小 SOLA 拼接的痕迹会显眼。
                # v2ProPlus 也不开 gpt_streaming：2080 Ti 上同一份权重实测 streaming_mode
                # 首帧 0.62~0.93 s / RTF 0.19~0.24，换 gpt_streaming 是 0.83~1.59 s /
                # RTF 0.19~0.38，还要为每个新形状补交一次 CUDA graph 捕获（预热后第一条
                # 真实回复的首帧 16.25 s），逐 token 流式在这台机器上没有收益。
                "min_chunk_length": 16,
                "overlap_length": 2,
            })
            for frag in gen:
                sr, chunk = frag[0], frag[1]
                if blocker is None:
                    blocker = _TtsBlocker(sr, out_sr, channels, block_seconds)
                for frame in blocker.push(chunk):
                    sent += 1
                    yield frame
        except Exception as e:
            # 引擎逐句推理，某一句抛异常时前面的帧已经发出去了。这里必须正常收尾，
            # 让 StreamingResponse 补上终止 chunk：异常逃出生成器的话 uvicorn 直接
            # 掐断连接，板侧读下一帧头拿到 -WEBCLIENT_DISCONNECT(-7)，会把一次截断
            # 报成链路断了。
            traceback.print_exc()
            print("[tts] stream stopped after %d blocks: %s: %s"
                  % (sent, type(e).__name__, e), file=sys.stderr)
        if blocker is not None:
            for frame in blocker.finish():
                yield frame


_TTS_TEXT_MAP = (
    ("\u00b0C", " degrees Celsius"),
    ("\u00b0F", " degrees Fahrenheit"),
    ("\u00b0", " degrees"),
    ("\u2248", " about "),
    ("\u00b1", " plus minus "),
    ("%", " percent"),
    ("\u2014", ", "),
    ("\u2013", ", "),
)
# 引擎的切句器把非 ASCII 字符一律按中日韩处理：°C 和破折号夹在英文里会单独切成一个
# 不含汉字的中文片段，规范化后一个 phone 都不剩，TextPreprocessor.get_bert_feature
# 里的 torch.cat([]) 抛 ValueError，整条流式响应被 uvicorn 掐断。上游只在整句层面过滤
# 纯符号（TextPreprocessor.preprocess），漏了句内片段。白名单留 ASCII（这些走 en 路径，
# 符号最多被丢掉）和真正的中日韩文字与中文标点。
_TTS_TEXT_SAFE = re.compile(
    r"[^ -~\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af\uff0c\u3002\u3001\uff1b\uff1a"
    r"\uff01\uff1f\uff08\uff09\u2018\u2019]+",
    re.S,
)


def _tts_sanitize(text: str) -> str:
    """把念不出来的符号换成单词，剩下的非中日韩符号削成空格。"""
    for bad, good in _TTS_TEXT_MAP:
        text = text.replace(bad, good)
    text = _TTS_TEXT_SAFE.sub(" ", text)
    return re.sub(r" {2,}", " ", text).strip()


class TTSRequest(BaseModel):
    text: str
    speaker: Optional[str] = None
    language: str = "zh"
    speed: float = 1.0
    sample_rate: Optional[int] = None
    channels: int = 1
    # stream=True 时响应体是 [u32 长度][WAV] 的帧序列（Transfer-Encoding: chunked），
    # 第一帧约 1 s 内就能到；默认 false 保持原来的整条 audio/wav
    stream: bool = False
    block_seconds: float = 1.5


# 不声明的话 FastAPI 按 application/json 生成 OpenAPI，Swagger UI 会拿 JSON 去解析
# 响应体，直接报解析失败，连下载链接都不给。
_TTS_WAV_RESPONSES = {
    200: {
        "description": "合成好的 WAV 字节流，响应头带 X-Speaker 与 X-Sample-Rate",
        "content": {"audio/wav": {"schema": {"type": "string", "format": "binary"}}},
    }
}


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


async def _tts_handle(text: str, speaker: Optional[str], language: str, speed: float,
                      sample_rate: Optional[int], channels: int,
                      stream: bool = False, block_seconds: float = 1.5) -> Response:
    """校验入参并合成，POST 与 GET 共用一条链，返回 audio/wav 响应。"""
    speakers = _tts_speakers()
    if not speakers:
        raise HTTPException(status_code=503, detail=_TTS_UNAVAILABLE)
    text = text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="text 不能为空")
    # 表情符号念不出来，切句后还白占一段推理
    text = re.sub(r"[\U00010000-\U0010FFFF]", "", text).strip()
    text = _tts_sanitize(text)
    if not text:
        raise HTTPException(status_code=400, detail="text 去掉表情符号和不可念的符号后为空")
    language = _tts_lang(language)
    if language not in _TTS_LANGS:
        raise HTTPException(status_code=400,
                            detail="language 只能是 " + "/".join(_TTS_LANGS)
                                   + "，或对应中文名 " + "/".join(_TTS_LANG_ALIAS))
    name = speaker or _tts_default(speakers)
    sp = speakers.get(name)
    if sp is None:
        raise HTTPException(status_code=404,
                            detail=f"音色 {name} 不存在，可用: {', '.join(sorted(speakers))}")
    if channels not in (1, 2):
        raise HTTPException(status_code=400, detail="channels 只能是 1 或 2")
    if not 0.5 <= block_seconds <= 5.0:
        raise HTTPException(status_code=400, detail="block_seconds 只能在 0.5~5.0 之间")
    if stream:
        # 生成器已经开了响应头才出错的话没法再改状态码，只能中断；板侧靠帧长度不足
        # 和收不到结束帧来判失败
        return StreamingResponse(
            _tts_stream_frames(sp, text, language, speed, sample_rate, channels, block_seconds),
            media_type="audio/wav",
            headers={"X-TTS-Framing": "u32le-wav", "X-Speaker": name},
        )
    try:
        # 推理是 GPU/CPU 密集同步调用，放线程池避免拖慢同进程的上传下载
        sr, wav = await asyncio.to_thread(_tts_synth_wav, sp, text,
                                         language, speed, sample_rate, channels)
    except HTTPException:
        raise
    except Exception as e:
        traceback.print_exc()
        raise HTTPException(status_code=500,
                            detail=f"tts failed: {type(e).__name__}: {e}")
    return Response(content=wav, media_type="audio/wav",
                    headers={"X-Sample-Rate": str(sr), "X-Speaker": name})


@app.post("/tts", summary="文本转语音，返回 audio/wav 字节",
          response_class=Response, responses=_TTS_WAV_RESPONSES)
async def tts(req: TTSRequest):
    return await _tts_handle(req.text, req.speaker, req.language, req.speed,
                             req.sample_rate, req.channels, req.stream,
                             req.block_seconds)


@app.get("/tts", summary="文本转语音（GET 版，供浏览器与 audio 标签直接用）",
         response_class=Response, responses=_TTS_WAV_RESPONSES)
async def tts_get(text: str, speaker: Optional[str] = None, language: str = "zh",
                  speed: float = 1.0, sample_rate: Optional[int] = None,
                  channels: int = 1):
    """浏览器地址栏打不开 POST，Swagger UI 也只给二进制响应挂下载链接、不放播放器。
    这条 GET 让 /tts?text=... 本身就是个可播放的音频 URL，试听页 /tts/play 依赖它。"""
    return await _tts_handle(text, speaker, language, speed, sample_rate, channels)


_TTS_PLAYER_HTML = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>TTS 试听</title>
<style>
  body { font: 14px/1.6 system-ui, sans-serif; max-width: 720px; margin: 24px auto; padding: 0 16px; }
  h1 { font-size: 18px; }
  label { display: inline-block; min-width: 84px; }
  input, select { font: inherit; padding: 3px 6px; }
  #text { width: 100%; box-sizing: border-box; margin: 8px 0; }
  .row { margin: 6px 0; }
  #status { color: #555; min-height: 1.6em; }
  audio { width: 100%; margin-top: 10px; }
  a { color: #06c; }
</style>
</head>
<body>
<h1>TTS 试听</h1>
<textarea id="text" rows="3">你好，我是大飛魚，今天天气不错。</textarea>
<div class="row"><label for="speaker">音色</label><select id="speaker"></select>
  <label for="language">语种</label><select id="language"></select></div>
<div class="row"><label for="sample_rate">采样率</label><input id="sample_rate" size="7" placeholder="不重采样">
  <label for="channels">声道</label><select id="channels"><option value="1">1</option><option value="2">2</option></select>
  <label for="speed">语速</label><input id="speed" size="4" value="1.0"></div>
<div class="row"><button id="go">合成并播放</button>
  <span id="status"></span></div>
<audio id="player" controls></audio>
<p><a id="dl" download="tts.wav">下载本次合成</a> · <a href="/tts?text=%E4%BD%A0%E5%A5%BD">GET /tts 直链示例</a></p>
<script>
const $ = id => document.getElementById(id);
const q = {};
const params = () => {
  const p = new URLSearchParams({ text: $("text").value, language: $("language").value,
                                  speed: $("speed").value || "1.0", channels: $("channels").value });
  if ($("speaker").value) p.set("speaker", $("speaker").value);
  if ($("sample_rate").value) p.set("sample_rate", $("sample_rate").value);
  return p;
};
fetch("/tts/speakers").then(r => r.json()).then(j => {
  const d = j.data || {};
  for (const s of d.speakers || []) {
    const o = document.createElement("option");
    o.value = o.textContent = s;
    if (s === d.default) o.selected = true;
    $("speaker").append(o);
  }
  for (const l of d.languages || []) {
    const o = document.createElement("option");
    o.value = l; o.textContent = d.language_alias[l] ? l + "（" + d.language_alias[l] + "）" : l;
    if (l === "zh") o.selected = true;
    $("language").append(o);
  }
}).catch(() => { $("status").textContent = "/tts/speakers 读取失败，接口可能未装配（503）"; });

$("go").onclick = async () => {
  $("go").disabled = true;
  $("status").textContent = "合成中，冷启动首帧可能要等几十秒……";
  const t0 = performance.now();
  try {
    const r = await fetch("/tts?" + params());
    if (!r.ok) {
      let detail = r.status + " " + r.statusText;
      try { detail = (await r.json()).detail || detail; } catch (e) {}
      throw new Error(detail);
    }
    const blob = await r.blob();
    if (q.url) URL.revokeObjectURL(q.url);
    q.url = URL.createObjectURL(blob);
    const audio = $("player");
    audio.src = q.url;
    audio.play();
    const link = $("dl");
    link.href = q.url;
    $("status").textContent =
      blob.size + " 字节，" + (r.headers.get("x-sample-rate") || "?") + " Hz，" +
      r.headers.get("x-speaker") + "，耗时 " + ((performance.now() - t0) / 1000).toFixed(1) + " s";
  } catch (e) {
    $("status").textContent = "失败：" + e.message;
  } finally {
    $("go").disabled = false;
  }
};
</script>
</body>
</html>
"""


@app.get("/tts/play", summary="浏览器试听页（/docs 里播不出声音，试听走这条）",
         response_class=HTMLResponse)
def tts_play():
    return HTMLResponse(_TTS_PLAYER_HTML)


@app.on_event("startup")
def _tts_preload():
    """TTS_AUTOLOAD=1 时启动就热一遍推理链路，否则第一条真实请求要等 50 s 上下。"""
    if os.getenv("TTS_AUTOLOAD", "0") != "1":
        return
    speakers = _tts_speakers()
    if not speakers:
        print("[tts] TTS_AUTOLOAD=1 但没有可用音色，跳过预加载", flush=True)
        return

    def _work():
        # 只建 pipeline 不算热：首次推理还要付文本前端（roberta/chinese-hubert）和声码器
        # 的 CUDA 冷启动。预热句要按真实回复的长度来：拿"预热。"这种两字废文本热的时候，
        # 启动后第一条 23 字回复首帧 5.53 s（v2ProPlus）；换成一句 31 字预热同一句 1.37 s，
        # 再往后 0.58~0.66 s。换音色那次请求另算，见 _tts_pipeline 的重载。
        import time

        sp = speakers[_tts_default(speakers)]
        t0 = time.perf_counter()
        try:
            n = sum(1 for _ in _tts_stream_frames(
                sp, "服务端把整条回复按一到两秒的块往外发，板子收到第一块就能开口说话。",
                "zh", 1.0, None, 1, 0.5))
            print(f"[tts] preload warm: {n} frames in {time.perf_counter() - t0:.1f} s",
                  flush=True)
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