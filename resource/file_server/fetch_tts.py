#!/usr/bin/env python3
"""Fetch the third-party pieces this repository does not carry.

    python fetch_tts.py check                  # only report what is missing
    python fetch_tts.py dicts voice assets     # fetch everything
    python fetch_tts.py assets --source hf     # pull zips from HuggingFace
    python fetch_tts.py dicts --mirror         # route GitHub through gh-proxy

Stdlib only, so it runs before any virtualenv exists. The inference code itself is
in third_party/gpt-sovits-infer (MIT, pinned upstream commit); what stays out are
the blobs: the 35 MB of dictionaries that upstream keeps in git, the pretrained
models, and the FFmpeg/nltk runtime. The voice that ships with the repo is our own
fine-tune; its two weight files live on ModelScope (the git host rejects them),
the reference audio and manifest stay in the repo.
See the 第三方与许可 section in README.md.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
# Inference code is vendored; this is the root it expects to be run from, and the
# place every fetched blob lands.
INFER = ROOT / "third_party" / "gpt-sovits-infer"
# Scratch for the zips being downloaded; kept out of the repo dir so an
# interrupted fetch can't leave a 1 GB archive that `git add .` would swallow.
TMP = ROOT / "_fetch_tmp"

UPSTREAM_COMMIT = "b4570b876f5c086819ba84e3215472733c6a7b70"
UPSTREAM_RAW = "https://raw.githubusercontent.com/RVC-Boss/GPT-SoVITS/" + UPSTREAM_COMMIT + "/"
# v2Pro voices need the ERes2NetV2 speaker encoder (sv.py:6). It ships inside
# pretrained_models.zip but the zip is skipped once chinese-hubert-base is there,
# so it gets its own marker.
SV_CKPT = INFER / "GPT_SoVITS/pretrained_models/sv/pretrained_eres2netv2w24s4ep4.ckpt"

# 自家微调的音色权重：单个 155 MB / 135 MB，GitHub 传不上去，放在 ModelScope。
# 仓库里的 refs/da_fei_yv/manifest.json 记录哈希，这里只列要拉的两个文件。
# 直链匿名可下，不需要 token。
VOICE_REPO = "https://www.modelscope.cn/models/Caoxiansong/da_fei_yv/resolve/master"
VOICE_WEIGHTS = {
    "GPT_weights_v2Pro/da_fei_yv-e40.ckpt":
        "d28c801091e668ebe0b3751b931d22c0c9b2824bf22919bafa54609b6bd39217",
    "SoVITS_weights_v2Pro/da_fei_yv_e6_s162.pth":
        "9872a80e4e1f82821ef679c67602eac46bbd9b7afecb9403ee7d0bf2f127449b",
}

# Checked into upstream's git but too big for ours. Path -> sha256 of that commit.
DICT_BLOBS = {
    "GPT_SoVITS/Accel/vendor/cuda/bin/cudart64_12.dll":
        "c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344",
    "GPT_SoVITS/text/namedict_cache.pickle":
        "559552094c4a6e995213e3fa586330e078ef8cb3a7a95a3109e945111cd2bfc1",
    "GPT_SoVITS/text/g2pw/polyphonic.pickle":
        "e00b04584ccc1453b0b23a272d5392e6e5a380a1c8beb3ab00765e0fea3fa1fa",
    "GPT_SoVITS/text/g2pw/polyphonic-fix.rep":
        "0d810a8734db881b44b310c1d31ff9646eb2ba71f9ddd155468c61ab335b7b57",
    "GPT_SoVITS/text/cmudict-fast.rep":
        "d6701402ea9249e4ae1e23ab8a4c216e847d49a30bd9bb206a33b3c7830f0a4a",
    "GPT_SoVITS/text/cmudict.rep":
        "a89a031728feb600a5f2f2c566f66d7e01c7efb7640ff307b9e3c28b82c8d4a9",
    "GPT_SoVITS/text/engdict_cache.pickle":
        "9bff9393f4b192d873a11335efc8f124771087b6dc847d34fd240c2846889d2b",
    "GPT_SoVITS/text/ja_userdic/userdict.csv":
        "cf3ae9063ee6b171f5553900fcaa813238a8d20bb0917a6a3a0f8d0c35c27962",
}

ZIP_SOURCES = {
    "modelscope": "https://www.modelscope.cn/models/XXXXRT/GPT-SoVITS-Pretrained/resolve/master",
    "hf": "https://huggingface.co/XXXXRT/GPT-SoVITS-Pretrained/resolve/main",
    "hf-mirror": "https://hf-mirror.com/XXXXRT/GPT-SoVITS-Pretrained/resolve/main",
}
LID_URL = "https://dl.fbaipublicfiles.com/fasttext/supervised-models/lid.176.bin"
LID_MD5 = "01810bc59c6a3d2b79c79e6336612f65"
FFMPEG_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-master-latest-win64-gpl-shared.zip"
)

FFMPEG_FILES = [
    "avcodec-63.dll",
    "avdevice-63.dll",
    "avfilter-12.dll",
    "avformat-63.dll",
    "avutil-61.dll",
    "swresample-7.dll",
    "swscale-10.dll",
    "ffmpeg.exe",
    "ffprobe.exe",
    "ffplay.exe",
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, cwd=None, env=None):
    print("  $ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.check_call(cmd, cwd=cwd, env=env)


def download(url, dest, mirror=False):
    if mirror and "github.com" in url:
        url = "https://gh-proxy.com/" + url
    dest.parent.mkdir(parents=True, exist_ok=True)
    curl = shutil.which("curl")
    if curl:  # resumable, which matters on flaky links
        run([curl, "-fL", "--retry", "3", "-C", "-", "-o", str(dest), url])
    else:
        print("  fetching " + url, flush=True)
        tmp = dest.with_suffix(dest.suffix + ".part")
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        tmp.replace(dest)
    return dest


def unzip_into(zip_path, target_dir):
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(target_dir)


# --------------------------------------------------------------------------- steps
def step_dicts(mirror):
    """Pull the dictionaries that upstream tracks in git but we keep out of the repo."""
    for rel, digest in sorted(DICT_BLOBS.items()):
        target = INFER / rel
        if target.is_file() and sha256(target) == digest:
            print("exists: " + rel)
            continue
        url = UPSTREAM_RAW + rel
        if mirror:
            url = "https://gh-proxy.com/" + url
        actual = sha256(download(url, target, mirror=False))
        if actual != digest:
            target.unlink(missing_ok=True)
            sys.exit("sha256 mismatch for " + rel + "\n  expected " + digest + "\n  actual   " + actual)
        print("fetched " + rel)


def step_assets(source, mirror):
    base = ZIP_SOURCES[source]
    jobs = [
        (base + "/pretrained_models.zip", INFER / "GPT_SoVITS",
         INFER / "GPT_SoVITS/pretrained_models/chinese-hubert-base/pytorch_model.bin"),
        (base + "/G2PWModel.zip", INFER / "GPT_SoVITS/text",
         INFER / "GPT_SoVITS/text/G2PWModel/g2pW.onnx"),
        (base + "/nltk_data.zip", INFER / "runtime",
         INFER / "runtime/nltk_data/corpora/cmudict/cmudict"),
    ]
    for url, into, marker in jobs:
        if marker.exists():
            print("exists: " + str(marker.relative_to(ROOT)))
            continue
        archive = TMP / Path(url).name
        download(url, archive, mirror)
        unzip_into(archive, into)
        archive.unlink()
        print("extracted into " + str(into.relative_to(ROOT)))

    if not SV_CKPT.exists():
        archive = TMP / "pretrained_models.zip"
        download(base + "/pretrained_models.zip", archive, mirror)
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if "/sv/" in n]
        if not names:
            archive.unlink()
            sys.exit("pretrained_models.zip 里没有 sv/ 成员，换 --source 再试")
        with zipfile.ZipFile(archive) as z:
            for n in names:
                z.extract(n, INFER / "GPT_SoVITS")
        archive.unlink()
        print("extracted %d sv/ files into GPT_SoVITS/pretrained_models/sv/" % len(names))
    else:
        print("exists: " + str(SV_CKPT.relative_to(ROOT)))

    lid = INFER / "GPT_SoVITS/pretrained_models/fast_langdetect/lid.176.bin"
    if lid.exists() and md5(lid) == LID_MD5:
        print("exists: " + str(lid.relative_to(ROOT)))
    else:
        lid.parent.mkdir(parents=True, exist_ok=True)
        download(LID_URL, lid, mirror)
        if md5(lid) != LID_MD5:
            sys.exit("md5 mismatch for lid.176.bin")

    runtime_bin = INFER / "runtime"
    if os.name == "nt":
        missing = [f for f in FFMPEG_FILES if not (runtime_bin / f).is_file()]
        if not missing:
            print("exists: runtime/ FFmpeg shared libraries")
        else:
            runtime_bin.mkdir(parents=True, exist_ok=True)
            archive = TMP / "ffmpeg-shared.zip"
            download(FFMPEG_URL, archive, mirror)
            with zipfile.ZipFile(archive) as z:
                names = [n for n in z.namelist() if Path(n).name in FFMPEG_FILES]
                for n in names:
                    with z.open(n) as src, open(runtime_bin / Path(n).name, "wb") as dst:
                        shutil.copyfileobj(src, dst)
            archive.unlink()
            print("extracted " + str(len(names)) + " FFmpeg files into runtime/")
    else:
        print("note: torchcodec on Linux/macOS uses the system FFmpeg; install ffmpeg-libs if loading fails")


def step_voice():
    """Pull our fine-tuned weights from ModelScope into the dirs TTS_Config reads."""
    for rel, digest in sorted(VOICE_WEIGHTS.items()):
        target = INFER / rel
        if target.is_file() and sha256(target) == digest:
            print("exists: " + rel)
            continue
        download(VOICE_REPO + "/" + rel, target)
        actual = sha256(target)
        if actual != digest:
            target.unlink(missing_ok=True)
            sys.exit("sha256 mismatch for " + rel + "\n  expected " + digest + "\n  actual   " + actual)
        print("fetched " + rel)


def step_check():
    probes = {
        "inference code": INFER / "GPT_SoVITS/TTS_infer_pack/TTS.py",
        "chinese-hubert-base": INFER / "GPT_SoVITS/pretrained_models/chinese-hubert-base/pytorch_model.bin",
        "chinese-roberta-wwm-ext-large": INFER / "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/pytorch_model.bin",
        "G2PWModel": INFER / "GPT_SoVITS/text/G2PWModel/g2pW.onnx",
        "fasttext LID": INFER / "GPT_SoVITS/pretrained_models/fast_langdetect/lid.176.bin",
        "sv ERes2NetV2 (v2Pro 用)": SV_CKPT,
        "nltk cmudict": INFER / "runtime/nltk_data/corpora/cmudict/cmudict",
        "FFmpeg (torchcodec)": INFER / "runtime/avcodec-63.dll" if os.name == "nt" else INFER / "runtime",
        "GPT weights": INFER / "GPT_weights_v2Pro/da_fei_yv-e40.ckpt",
        "SoVITS weights": INFER / "SoVITS_weights_v2Pro/da_fei_yv_e6_s162.pth",
        "reference audio": INFER / "refs/da_fei_yv/dwg_0002.wav",
    }
    for rel in sorted(DICT_BLOBS):
        probes["dict " + rel.split("/")[-1]] = INFER / rel
    bad = [k for k, p in probes.items() if not p.exists()]
    for k, p in probes.items():
        print(("  ok    " if p.exists() else "  absent ") + k + "  ->  " + str(p.relative_to(ROOT)))
    if bad:
        print("\nmissing: " + ", ".join(bad))
        return 1
    print("\nall third-party pieces present")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", nargs="*", choices=["dicts", "voice", "assets", "check"], default=["check"])
    ap.add_argument("--source", choices=sorted(ZIP_SOURCES), default="modelscope")
    ap.add_argument("--mirror", action="store_true", help="route github.com downloads through gh-proxy.com")
    a = ap.parse_args()

    if not a.steps:
        a.steps = ["check"]
    for s in a.steps:
        if s == "dicts":
            step_dicts(a.mirror)
        elif s == "voice":
            step_voice()
        elif s == "assets":
            step_assets(a.source, a.mirror)
    return step_check() if "check" in a.steps or a.steps == ["check"] else 0


if __name__ == "__main__":
    sys.exit(main())
