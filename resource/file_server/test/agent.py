"""
LLM 多模态端到端测试：把文件服务器上已上传的图片，以公网 URL 形式交给视觉模型。

使用前请确认：
  1. FILE_ID 与服务器 file_db.sqlite 中 file_record.file_id 完全一致（36 位 UUID，勿截断）；
  2. IMAGE_HOST 是内网穿透后的公网域名（不能用 127.0.0.1，LLM 服务端无法访问你的内网）；
  3. 穿透域名证书必须被公网信任（浏览器访问无证书告警）；
  4. 脚本会自动先做下载预检（check_image_url），确认返回 200 且 Content-Type 为 image/*。
"""
import urllib.request
from openai import OpenAI

# 内网穿透后的公网主机（不要用 127.0.0.1）
IMAGE_HOST = "your-host"
# 与服务器 file_db.sqlite 完全一致的 file_id（复制完整，勿截断末尾字符）
FILE_ID = "your-file-id"
API_KEY = "your-api-key"
IMAGE_URL = f"{IMAGE_HOST}/download/{FILE_ID}"


def check_image_url(url: str) -> None:
    """调用 LLM 前先验证 URL 可下载且为图片，避免 400 难排查。"""
    try:
        req = urllib.request.Request(url, method="GET", headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=20) as resp:
            ctype = resp.headers.get("Content-Type", "")
            print(f"[check] status={resp.status} Content-Type={ctype}")
            if resp.status != 200 or not ctype.startswith("image/"):
                raise SystemExit(
                    f"[check] 下载失败或非图片响应：status={resp.status}, Content-Type={ctype}\n"
                    "请检查 FILE_ID 是否完整、隧道是否可达、证书是否被公网信任"
                )
    except Exception as e:
        raise SystemExit(
            f"[check] 无法下载图片 URL：{e}\n"
            "1) FILE_ID 是否与服务器数据库完全一致（36 位 UUID，勿截断）\n"
            "2) 隧道是否运行、域名证书是否被公网信任（浏览器无告警）\n"
            "3) 本地 curl -I <url> 是否返回 200 与 Content-Type: image/*"
        )


check_image_url(IMAGE_URL)

client = OpenAI(
    api_key=API_KEY,
    base_url="https://api.siliconflow.cn/v1"
)

response = client.chat.completions.create(
    model="Qwen/Qwen3.5-9B",
    messages=[
        {
            "role": "user",
            "content": [
                {
                    "type": "image_url",
                    "image_url": {
                        "url": IMAGE_URL
                    }
                },
                {
                    "type": "text",
                    "text": "这张图片显示的是什么样。"
                }
            ]
        }
    ]
)

print(response.choices[0].message.content)
