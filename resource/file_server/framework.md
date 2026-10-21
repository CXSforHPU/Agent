# 文件服务器（file_server）

FastAPI 实现的嵌入式文件服务器：接收 RT-Thread webclient 的 multipart 流式上传，按 `file_id` 提供下载与元信息查询，并用本地 whisper 提供语音转写。

## 启动

```bash
cd resource/file_server
uv run main.py
# 或：python main.py
```

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/upload` | multipart 上传，form 字段 `name=file`；返回 JSON 含 `file_id` |
| GET | `/download/{file_id}` | 下载文件（Content-Type 魔数优先、扩展名兜底，inline 展示） |
| GET | `/files/{file_id}` | 查询单文件元信息 |
| GET | `/files` | 全部文件列表 |
| DELETE | `/files/{file_id}` | 删除文件 |
| POST | `/stt` | multipart 上传音频，form 字段 `name=file`；本地 whisper 转写，返回 `{"code":0,"data":{"text":"..."}}` |

## 重要：下载 URL 的拼装规则

- `/upload` 只返回 `file_id` 等信息，不返回完整下载 URL。
- 浏览器直接访问的地址是 `{服务根地址}/download/{file_id}`，本地为 `http://127.0.0.1:8000/download/{file_id}`。
- 内网穿透后服务根地址换成公网域名（如 `https://files.l2.bb1a.cn`），下载地址为 `{根地址}/download/{file_id}`。
- 嵌入式侧把宏 `PKG_AGENT_DOWNLOAD_URL` 配到 `/download` 为止，例如 `http://127.0.0.1:8000/download`。
  `utils.c::agent_multi_get_file` 和 `agent_upload_file` 会直接追加 `/{file_id}`，拼成
  `{PKG_AGENT_DOWNLOAD_URL}/{file_id}`，不要再重复 `/download`。

## 多模态（LLM 抓取图片 URL）注意事项

1. **`file_id` 必须与数据库完全一致**（36 位 UUID）。少一位即 404，LLM 服务端抓取到 404
   错误页（非图片）会报 `400 code 20040 The image URL must be a valid and downloadable URL`。
2. **LLM 服务端无法访问你的内网**：基址必须使用公网可达的穿透域名，不能用 `127.0.0.1`。
3. **证书必须被公网信任**：穿透使用自签/私有 CA 证书会导致 LLM 服务端 TLS 握手失败，同样报 400。
4. **Content-Type 必须正确**：`main.py` 先按魔数识别 JPEG/PNG/GIF/WebP/BMP 与 WAV/AAC/MP3/AMR/OGG/FLAC，
   未命中时按扩展名兜底，仍无法判断时返回 `application/octet-stream`。`.amr` 和 `.raw` 依赖魔数，避免被误判。
5. **验证方法**（任选其一）：
   - 浏览器直接打开 `https://你的域名/download/{file_id}`，应直接显示图片且无证书告警；
   - `curl -I https://你的域名/download/{file_id}` 应返回 `200` 且 `Content-Type: image/jpeg` 等。

## 测试脚本（test/）

- `upload_client.py` / `download_client.py` / `list_client.py`：接口自测
- `agent.py`：多模态端到端测试（公网图片 URL → 视觉模型），内含下载预检
  （`check_image_url` 会在调用 LLM 前先确认 URL 可下载且为图片，失败时给出排查提示）
