#!/usr/bin/env python3
"""从上游整仓 clone 里抽推理子集，复制到 third_party/gpt-sovits-infer（维护者工具）。

上游 clone 只是抽代码时的输入，不入库：先在 file_server 目录跑
`git clone https://github.com/RVC-Boss/GPT-SoVITS.git GPT-SoVITS`
并 checkout 到 fetch_tts.py 里钉住的 UPSTREAM_COMMIT。

    python extract_infer.py            # 干跑，打印三类清单
    python extract_infer.py --apply    # 复制进 third_party/gpt-sovits-infer

判定依据是一次真实合成（zh，v2，大肥鱼权重）在 sys.modules 里留下的上游 .py，
共 97 个；这里按目录规则放宽到 166 个，补上其它语种的文本模块和包的 __init__。
训练、数据预处理、导出、WebUI、ASR、人声分离全部丢掉。
大于 500 KB 的数据文件不进仓库，改由 fetch_tts.py 的 dicts 步骤取。
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "GPT-SoVITS"
DST = ROOT / "third_party" / "gpt-sovits-infer"
SCOPES = ("GPT_SoVITS/", "tools/")

# 训练与数据准备
DROP_PREFIX = (
    "GPT_SoVITS/AR/data/",
    "GPT_SoVITS/prepare_datasets/",
    "GPT_SoVITS/s1_train.py",
    "GPT_SoVITS/s2_train.py",
    "GPT_SoVITS/s2_train_v3.py",
    "GPT_SoVITS/s2_train_v3_lora.py",
    "GPT_SoVITS/module/losses.py",
    "GPT_SoVITS/module/data_utils.py",
    # 导出与 onnx 变体：只有 export 脚本会 import
    "GPT_SoVITS/export_torch_script.py",
    "GPT_SoVITS/export_torch_script_v3v4.py",
    "GPT_SoVITS/onnx_export.py",
    "GPT_SoVITS/Accel/PyTorch/AR/export.py",
    "GPT_SoVITS/AR/models/t2s_model_onnx.py",
    "GPT_SoVITS/AR/models/t2s_lightning_module_onnx.py",
    "GPT_SoVITS/AR/models/t2s_model_cudagraph.py",
    "GPT_SoVITS/AR/models/embedding_cudagraph.py",
    "GPT_SoVITS/AR/models/structs_cudagraph.py",
    "GPT_SoVITS/AR/modules/activation_onnx.py",
    "GPT_SoVITS/AR/modules/embedding_onnx.py",
    "GPT_SoVITS/AR/modules/patched_mha_with_cache_onnx.py",
    "GPT_SoVITS/AR/modules/transformer_onnx.py",
    "GPT_SoVITS/module/attentions_onnx.py",
    "GPT_SoVITS/module/models_onnx.py",
    "GPT_SoVITS/module/v5_inference.py",
    # 上游自带的推理外壳：我们自己写了 /tts
    "GPT_SoVITS/inference_webui.py",
    "GPT_SoVITS/inference_webui_fast.py",
    "GPT_SoVITS/inference_gui.py",
    "GPT_SoVITS/inference_cli.py",
    "GPT_SoVITS/download.py",
    "GPT_SoVITS/stream_v2pro.py",
    # 由 fetch_tts.py 取走的模型目录
    "GPT_SoVITS/text/G2PWModel/",
    "GPT_SoVITS/pretrained_models/",
    # 训练侧配置；tts_infer.yaml 由 TTS_Config 首次运行时生成，不入库
    "GPT_SoVITS/configs/s1",
    "GPT_SoVITS/configs/train.yaml",
    "GPT_SoVITS/configs/tts_infer.yaml",
    # 数据集预处理与音频工具链
    "tools/asr/",
    "tools/uvr5/",
    "tools/denoise-model/",
    "tools/slice_audio.py",
    "tools/slicer2.py",
    "tools/cmd-denoise.py",
    "tools/subfix_webui.py",
    "tools/assets.py",
    "tools/my_utils.py",
    "tools/i18n/scan_i18n.py",
)
BLOB_LIMIT = 500 * 1024


def tracked():
    out = subprocess.check_output(["git", "-C", str(SRC), "ls-files", *SCOPES], text=True)
    return [l for l in out.splitlines() if l.strip()]


def dropped(rel):
    return any(rel == p or rel.startswith(p) for p in DROP_PREFIX)


def main():
    if not (SRC / ".git").is_dir():
        sys.exit("missing upstream clone: " + str(SRC))
    apply = "--apply" in sys.argv
    keep, blobs, drop, absent = [], [], [], []
    for rel in tracked():
        p = SRC / rel.replace("/", os.path.sep)
        if not p.is_file():
            # ls-files 会列出工作区里已被移走或删掉的路径，计数报出来，别静默跳过
            absent.append(rel)
            continue
        if dropped(rel):
            drop.append(rel)
        elif p.stat().st_size >= BLOB_LIMIT:
            blobs.append((p.stat().st_size, rel))
        else:
            keep.append(rel)

    size = sum((SRC / k.replace("/", os.path.sep)).stat().st_size for k in keep)
    print("KEEP %d files (%d .py), %.2f MB" % (len(keep), len([k for k in keep if k.endswith(".py")]), size / 1e6))
    print("BLOB %d files, %.1f MB -> fetch_tts.py dicts" % (len(blobs), sum(s for s, _ in blobs) / 1e6))
    for s, rel in sorted(blobs, reverse=True):
        print("       %9d  %s" % (s, rel))
    print("DROP %d files" % len(drop))
    if absent:
        print("ABSENT %d tracked paths missing from the working tree" % len(absent))
        for rel in sorted(absent)[:12]:
            print("       " + rel)
    if not apply:
        print("\ndry run; add --apply to copy into " + str(DST))
        return

    for rel in keep:
        src = SRC / rel.replace("/", os.path.sep)
        dst = DST / rel.replace("/", os.path.sep)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not (dst.is_file() and dst.stat().st_size == src.stat().st_size):
            subprocess.check_call(["cp", str(src), str(dst)])
    print("copied %d files into %s" % (len(keep), DST))


if __name__ == "__main__":
    main()
