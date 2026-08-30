from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.responses import FileResponse
import os
import uuid
import sqlite3
import mimetypes
from datetime import datetime
from typing import Dict, Any

app = FastAPI(title="嵌入式流式文件服务", version="1.0")

UPLOAD_DIR = "./upload_files"
DB_PATH = "./file_db.sqlite"
os.makedirs(UPLOAD_DIR, exist_ok=True)


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
    if not os.path.exists(item["store_path"]):
        raise HTTPException(status_code=404, detail="file lost on disk")

    return FileResponse(
        path=item["store_path"],
        filename=item["original_name"],
        media_type=guess_media_type(item["store_path"]),
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

    if os.path.exists(item["store_path"]):
        os.remove(item["store_path"])
    cur.execute("DELETE FROM file_record WHERE file_id = ?", (file_id,))
    conn.commit()
    conn.close()
    return {"code": 0, "msg": "deleted"}


if __name__ == "__main__":
    import uvicorn
    # 限制最大请求50MB，根据你的场景修改
    uvicorn.run(
        app,
        host="0.0.0.0",
        port=8000,
    )