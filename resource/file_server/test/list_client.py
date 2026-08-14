import requests

BASE = "http://127.0.0.1:8000"

def list_files(page=1, size=20):
    resp = requests.get(f"{BASE}/files", params={"page":page, "size":size})
    resp.raise_for_status()
    data = resp.json()
    print(data)
    return data


if __name__ == "__main__":
    list_files()