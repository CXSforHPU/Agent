import json
import os

import requests


def upload_file(file_path):
    base = os.environ.get("FILE_SERVER_URL", "http://127.0.0.1:8000")
    url = base.rstrip("/") + "/upload"
    try:
        with open(file_path, "rb") as f:
            resp = requests.post(url, files={"file": f}, timeout=60)
            resp.raise_for_status()
            return resp.json()
    except requests.exceptions.RequestException as e:
        print(f"请求异常: {e}")
        return None
    except json.JSONDecodeError:
        print("响应不是有效的 JSON，原始文本:", resp.text)
        return None


if __name__ == "__main__":
    print(upload_file("./index.html"))
