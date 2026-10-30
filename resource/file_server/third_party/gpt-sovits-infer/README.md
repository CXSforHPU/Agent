# gpt-sovits-infer

上游 GPT-SoVITS 的推理子集，只保留合成一条链。训练、数据预处理、权重导出、WebUI、ASR、人声分离都不在这里。

## 出处

- 仓库：https://github.com/RVC-Boss/GPT-SoVITS
- 提交：`b4570b876f5c086819ba84e3215472733c6a7b70`（2026-10-19，fix(streaming): 支持 API v2 语义流式文本切分 #2850）
- 许可：MIT，`LICENSE` 原样保留，版权归 RVC-Boss
- 文件内容未改动，只做取舍。改动过哪个文件就记在这里，目前没有。
- 158 个文件与上游 commit 比，LF 归一后逐字节相同，差异只剩行尾：上游 clone 用 `core.autocrlf=true` checkout，落地是 CRLF；`git add` 时本仓库的 autocrlf 再折回 LF。折回去的结果是 129 个 blob 哈希与上游一致，剩下 29 个（`Accel/` 下的引擎文件、`text/korean.py`、`text/cantonese.py`、`process_ckpt.py`、`sv.py`、`tools/audio_sr.py` 等）上游自己就把 CRLF 提交进了 git，入库后只差行尾。

## 怎么选出来的

判据是运行时真实加载的文件，不是目录猜测。跑一次中文合成（v2 权重、大肥鱼音色），记录 `sys.modules` 里属于上游的 .py，共 97 个；再按目录规则放宽，补上其它语种的文本模块和各包的 `__init__.py`。这样落进来的是 158 个上游文件（119 个 .py，1.38 MB），加上上游 `LICENSE`、本说明、一份音色注册表和自家音色的 6 个文件，仓库里一共 167 个文件。

重抽或升级用上层的维护脚本：

```sh
cd ../..                      # file_server 目录
git clone https://github.com/RVC-Boss/GPT-SoVITS.git GPT-SoVITS
git -C GPT-SoVITS checkout <fetch_tts.py 里钉住的 UPSTREAM_COMMIT>
python extract_infer.py          # 干跑，打印 KEEP/BLOB/DROP/ABSENT 四类清单
python extract_infer.py --apply  # 复制进来
```

`extract_infer.py` 里的 `DROP_PREFIX` 就是取舍清单，改这里等于改抽取规则。大于 500 KB 的文件判为 BLOB，不复制，改由 `fetch_tts.py dicts` 取。`--apply` 只覆盖同名文件，不会删掉上游已经移除的路径，升级时要自己核对 DROP 清单。输入的那份整仓 clone 只是中间产物，`.gitignore` 已忽略 `GPT-SoVITS/`，抽完就可以删。

## 丢掉的模块

| 类别 | 路径 |
|---|---|
| 训练 | `s1_train.py`、`s2_train*.py`、`prepare_datasets/`、`AR/data/`、`module/losses.py`、`module/data_utils.py`、`configs/s1*.yaml`、`configs/train.yaml` |
| 导出 | `onnx_export.py`、`export_torch_script*.py`、`Accel/PyTorch/AR/export.py`、各 `*_onnx.py` 模块 |
| 上游推理外壳 | `inference_webui.py`、`inference_webui_fast.py`、`inference_gui.py`、`inference_cli.py`、`stream_v2pro.py`、`api.py`、`api_v2.py`、`webui.py` |
| 音频工具链 | `tools/asr/`、`tools/uvr5/`、`tools/slice_audio.py`、`tools/slicer2.py`、`tools/cmd-denoise.py`、`tools/my_utils.py` |

## 不入库的依赖

体积大的第三方资产不入库，`fetch_tts.py` 取的见 `../../fetch_tts.py`：

| 内容 | 大小 | 来源 |
|---|---|---|
| `GPT_SoVITS/text/` 下的英文与日文词表、`Accel/vendor/cuda/` | 8 个文件 35.3 MB | 上游同 commit 的源码压缩包（`dicts` 步骤） |
| `GPT_SoVITS/pretrained_models/` | 4.6 GB | GPT-SoVITS-Pretrained（modelscope / hf / hf-mirror） |
| `GPT_SoVITS/text/G2PWModel/` | 608 MB | 同上 |
| `runtime/`（nltk_data、FFmpeg） | 231 MB | 同上与 BtbN 构建 |

取完之后整棵树 5.8 GB，代码只占 1.4 MB。

## 入库的自家音色

`GPT_weights_v2Pro/`、`SoVITS_weights_v2Pro/`、`refs/` 与 `tts_speakers/` 入库，是本工程自训的 `dafeiyu2_v2Pro` 音色：两个权重分别是 148 MB 与 129 MB，走 Git LFS，仓库里存的是指针；参考音频 0.3 MB 直接入库。训练数据是 CC BY-NC 4.0，权重是衍生作品，同样受非商业与署名约束，详见 `../../README.md` 的「第三方与许可」。`GPT_weights_v2/` 与 `SoVITS_weights_v2/` 留空、放第三方 v2 权重用，被 `.gitignore` 挡住。

`GPT_SoVITS/configs/tts_infer.yaml` 不入库。上游 `TTS_Config` 每次建管线都会把解析后的权重绝对路径写回这个文件，文件不在就先生成一份默认的（`TTS_infer_pack/TTS.py:451` 的 `_load_configs`）。跟踪进来只会让工作区一直是脏的。
