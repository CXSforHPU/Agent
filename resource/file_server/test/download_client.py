import requests

BASE = "http://127.0.0.1:8000"

def download(file_id: str, save_as: str):
    url = f"{BASE}/download/{file_id}"
    resp = requests.get(url, stream=True)
    resp.raise_for_status()
    with open(save_as, "wb") as f:
        for chunk in resp.iter_content(chunk_size=8192):
            f.write(chunk)
    print(f"已保存到 {save_as}")


if __name__ == "__main__":
    # 把这里替换成上传得到的file_id
    download("7ae1b454-816d-4b67-b7cb-af2446b1b596", "out.jpg")