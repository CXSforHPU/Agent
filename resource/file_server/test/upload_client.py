import requests

import requests
import json

def upload_file(file_path):
    url = "http://files.l2.bb1a.cn/upload"
    try:
        with open(file_path, "rb") as f:
            files = {"file": f}
            resp = requests.post(url, files=files, timeout=60)  # 可适当增大超时
            resp.raise_for_status()
            return resp.json()
    except requests.exceptions.RequestException as e:
        print(f"请求异常: {e}")
        return None
    except json.JSONDecodeError:
        print("响应不是有效的 JSON，原始文本:", resp.text)
        return None

if __name__ == "__main__":
    ret = upload_file("./index.html")
    print(ret)