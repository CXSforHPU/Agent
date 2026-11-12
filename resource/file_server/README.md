# 文件服务器搭建

文件服务器为 Agent 的多模态功能提供文件上传、下载，以及本地语音转写。只使用纯文本对话时不需要部署。

## 1. 环境准备

装环境只要 uv，依赖和资产全部交给第 2 节的部署脚本，不需要手工执行 `uv sync`。安装 uv：

```bash
pip install uv
```

下载慢时可以换清华源：

```bash
pip install uv -i https://pypi.tuna.tsinghua.edu.cn/simple
```

## 2. 安装依赖

装环境和起服务统一用工程下的两个脚本，不要手工执行 `uv sync`：

```bash
cd packages/Agent-latest/resource/file_server
bash deploy.sh --tts      # Linux / macOS，Windows 下 Git Bash 也可
deploy.bat --tts          # Windows cmd
```

一条命令跑完四步：按算力装 torch、拉不入库的第三方资产、逐项核对 SHA256、起服务。四步都幂等，已经落地的文件跳过，重跑一遍很便宜。

按组件装依赖用 `--install`，组件名逗号分隔，取值 `file-server`、`mcp`、`tts`、`stt`：

```bash
bash deploy.sh --install file-server,stt --check   # 只装文件服务器与语音转写的依赖
deploy.bat --install file-server,tts --check       # Windows cmd
```

`--install` 只装依赖不起服务，后面带 `--check` 就行。`file-server` 是其它组件的地基，不管点名了哪些都在要装的清单里。`mcp` 没有自己的 Python 依赖，网关只用标准库，装它只是顺带看一眼 Node.js 与 Windows-MCP 在不在。

不想敲命令的话，管理控制台的运维页上有对应的按钮，详情见 [../web/README.md](../web/README.md) 的「运维页」一节。

### 参数

两个脚本的参数对得上，只有 GitHub 下载的默认走法不同。

| 参数 | 作用 |
|---|---|
| `--tts` | 装 `tts` 依赖组，并拉词表、权重与底模。不带这个参数时，脚本先查 `.venv` 里有没有 `librosa`，装过的环境按 `--tts` 处理 |
| `--install LIST` | 只装点名组件的依赖，逗号分隔。`file-server` 一定在里面 |
| `--start LIST` | 只起点名组件，逗号分隔。`file-server` 一定在里面 |
| `--status` | 打印各组件的依赖清单，不装也不起。需要 `.venv` 在 |
| `--json` | 配合 `--status` 用，输出机器读的清单 |
| `--check` | 只装配和自检，不起服务 |
| `--start-only` | 跳过装配与拉取，直接起服务 |
| `--print` | 只打印要执行的 `uv` 命令，什么都不改 |
| `--port N`、`--host ADDR` | 换监听地址，默认 `0.0.0.0:8000` |
| `--autoload` | 启动时在后台预热 TTS 模型，省掉首条请求的冷启动 |
| `--speaker NAME` | 默认音色，写进 `TTS_SPEAKER`。不设时服务端取 `da_fei_yv_plus` |
| `--cpu`、`--cu126`、`--cu130` | 指定 torch 后端，默认按显卡探测 |
| `--mirror` | GitHub 下载走 gh-proxy，`deploy.bat` 默认开，`deploy.sh` 默认关 |
| `--no-mirror` | 只有 `deploy.bat` 有，关掉代理直连 github.com |
| `--no-mcp` | 只起文件服务器，不起 MCP 网关 |
| `--mcp-port N` | MCP 网关的监听端口，默认 `8765` |

脚本取 `nvidia-smi --query-gpu=compute_cap`，按结果同步 `pyproject.toml` 里的依赖组：

| 检测结果 | 依赖组 | torch 来源 |
| --- | --- | --- |
| 没有 NVIDIA 显卡 | `cpu`（默认） | 清华 PyPI 的 CPU / MPS 构建 |
| 算力 5.0 ~ 9.x | `gpu-cu126` | `download.pytorch.org/whl/cu126` |
| 算力 10.0 及以上（Blackwell） | `gpu-cu130` | `download.pytorch.org/whl/cu130` |

cu126 的预编译核函数止于 sm_90，50 系显卡装了会在推理时报 no kernel image is available，所以要分开。

### 注意事项

- 首次装配要几 GB 磁盘。`--tts` 拉完资产后 `third_party/gpt-sovits-infer/` 占 6.1 GB，`.venv` 占 5.2 GB。
- `deploy.bat` 必须用 CRLF 行尾保存。cmd 找标签时按 CRLF 断行，文件一大，LF 行尾会让 `goto` 报“系统找不到指定的批处理文件”。本仓库的 `core.autocrlf` 是 true，检出时会是 CRLF，手工改完记得确认。
- 国内直连 GitHub 的原始文件会被断开，`deploy.bat` 默认走 gh-proxy。实测 80 KB/s ~ 400 KB/s，88 MB 的 zip 要等几分钟。
- cmd 会把逗号分隔的列表拆成一个个参数，`--install file-server,stt` 到脚本里成了 `%2` 是 `file-server`、`%3` 是 `stt`。脚本把这个列表拼回去，所以人和程序都能这么写。
- Windows 上文件服务器在跑时装依赖会失败。`uv sync` 换不了 `torch/lib/c10.dll`，报“拒绝访问”（os error 5）。先把文件服务器停掉再装。

### 为什么不要手工执行 uv sync

`uv sync` 只保留命令行点名的依赖组，其余全部卸掉。三条常见的手工命令各踩一个坑：

| 手工命令 | 后果 |
|---|---|
| `uv sync` | 默认组是 `cpu`，GPU 机器上的 `torch` 换回 CPU 构建；`tts` 组那 37 项连带 125 个依赖一起卸掉，`/tts` 返回 503 |
| `uv sync --no-default-groups --group gpu-cu126` | 后端保住了，`tts` 组仍然整个卸掉 |
| 任意一次 `uv sync` | `eunjeon` 与 `setuptools` 退回锁文件状态，`setuptools` 从 80.10.2 回到 84.0.0，合成韩语时报 `TypeError: exceptions must derive from BaseException` |

脚本这三处都挡住了。同步前用 `.venv` 里的 Python 查一次 `librosa` 的分发元数据，查到就自动带上 `--group tts`；`librosa` 只从 `tts` 组进环境，所以它是那组装没装过的判据。拉完资产后再查一次 `eunjeon` 与 `setuptools` 的元数据，缺了补回去。后端组由显卡探测决定，不会把 GPU 机器装成 CPU 构建。

确实要手工执行时，把组给全：`uv sync --no-default-groups --group gpu-cu126 --group tts`。脚本背后跑的就是下面这几条，读依赖组配置时用得上：

```bash
uv sync                                        # 无 GPU
uv sync --group tts                            # 无 GPU + GPT-SoVITS 推理链
uv sync --no-default-groups --group gpu-cu126  # CUDA 12.6
uv sync --no-default-groups --group gpu-cu126 --group tts
uv sync --no-default-groups --group gpu-cu130  # CUDA 13.0（Blackwell）
```

### 依赖组

`cpu` / `gpu-cu126` / `gpu-cu130` 三组只决定 torch 的后端，`tts` 组与它们正交：文件服务器本体只需要 `fastapi`、`openai`、`requests`、`openai-whisper`、`soundfile`、`numpy`、`resampy`，GPT-SoVITS 那套（`gradio`、`transformers`、`funasr`、`modelscope`、`onnxruntime-gpu` 等）全部在 `tts` 组里，不带 `--group tts` 就不会装。装配这条链的完整步骤见第 11 节。

三个后端组在 `[tool.uv]` 的 `conflicts` 里声明为互斥，`uv sync --group gpu-cu126` 会直接报错，必须先 `--no-default-groups` 关掉默认的 `cpu` 组。

默认组设成 `cpu`，目的是让不带参数的 `uv sync` 在没有 GPU 的机器上也能装出可用环境，不会误拉 2 GB 的 CUDA 包。副作用就是上表第一行：GPU 机器上裸跑一次 `uv sync`，torch 和整条推理链一起没。

依赖中包含 `openai-whisper`，它会拉取 torch，体积约 2 GB。只需要文件上传下载、不需要语音转写时，可以自行删除 `pyproject.toml` 里的 `openai-whisper`、`soundfile`、`numpy`、`resampy`，并去掉 `/stt` 接口。

### 组件清单与依赖探测

这套服务拆成四个组件，装依赖与起服务都按组件点名：

| 组件 | 显示名 | 依赖 |
|---|---|---|
| `file-server` | 文件服务器 | 虚拟环境，FastAPI、uvicorn、pydantic、starlette 四个包 |
| `mcp` | MCP 网关 | 网关脚本与网关代码包。网关只用 Python 标准库，没有额外的包 |
| `tts` | 语音合成 | torch、librosa、transformers、modelscope，GPT-SoVITS 推理代码、预训练模型、音色权重、FFmpeg 运行库 |
| `stt` | 语音转写 | openai-whisper、soundfile、resampy 三个包，whisper 模型文件 |

`components.py` 回答某个组件的依赖齐了没有。它只看文件和已装的包，不下载任何东西：

```bash
python components.py status          # 人看的清单
python components.py status --json   # 机器读的清单
```

人看的清单里每个依赖给三种标记：必需依赖齐了是 `ok`，缺了是 `缺`，可选依赖两种情况都给 `可选`。部署脚本带 `--status` 走的是同一份探测：

```bash
bash deploy.sh --status
deploy.bat --status --json
```

探测结果缓存在 `.components_cache.json`，有效期一小时。装完依赖脚本会删掉这个文件，下一次探测重新算。

### 镜像与索引

清华源已是默认索引，`uv.lock` 同时锁定三种 torch 构建，换机器不需要重新解析。非国内网络可以删掉 `pyproject.toml` 中名为 `tuna` 的那段 `[[tool.uv.index]]`，回到 PyPI。

torch 生态的三个包按硬件走不同索引：

- `torch`、`torchcodec`：GPU 组走 pytorch 官方索引，带 `+cuXXX` 后缀；`cpu` 组走清华 PyPI。
- `torchaudio`：始终走 PyPI。cu126 / cu130 索引上的 torchaudio 最高到 2.11.0，PyPI 有 2.11.1，实测与 `torch 2.14.1+cu126` 搭配可用。

索引策略设成 `unsafe-best-match` 是必须的：`+cu126` 属于 local version，默认的 first-index-wins 策略会在命第一个索引后停住，解不到带后缀的构建。

`download.pytorch.org` 慢时，可以把 `pytorch-cu126` / `pytorch-cu130` 两段的 `url` 前缀换成 `https://mirror.sjtu.edu.cn/pytorch-wheels`（实测是逐包目录的标准索引，`cu126/torch/` 里有同一个 `torch-2.14.1+cu126-cp312-win_amd64.whl`，`cu126/torchcodec/` 也在）。阿里 `mirrors.aliyun.com/pytorch-wheels/cu126/` 是一整页平铺的 wheel 清单，不是 PEP 503 索引，uv 用不了。换索引后 `uv lock` 会重写 `source` 字段，属于预期内的改动。

解释器本身不由清华提供：`python-build-standalone` 只在 GitHub Releases，uv 须自身下载 Python 时走不了清华源。若本机已有 Python 3.12，uv 会直接复用；被墙又无解释器时，启用 `pyproject.toml` 里注释掉的 `python-install-mirror`（第三方代理，非官方镜像，按需取舍）。

## 3. 启动服务

起服务也走脚本，`--start-only` 跳过装配与拉取，几秒就能起来：

```bash
bash deploy.sh --start-only          # Linux / macOS
deploy.bat --start-only              # Windows cmd
```

只起一部分组件用 `--start`，组件名逗号分隔：

```bash
bash deploy.sh --start-only --start file-server,stt
deploy.bat --start-only --start file-server,mcp
```

没点名的组件不注册端点。`tts` 没选时 `/tts` 返回 404，`stt` 没选时 `/stt` 返回 404，`mcp` 没选时网关端口不监听。`file-server` 是本体，永远在启动清单里。

换端口加 `--port 8001`，想让服务一起来就把模型预热好加 `--autoload`，换默认音色加 `--speaker da_fei_yv`。环境还没装配过时用 `bash deploy.sh --tts`，装完自动接着启动。

管理控制台的运维页上点「按勾选启动」走的是同一个 `--start-only` 加 `--start`，点「停止」会连 MCP 网关一起带走。

直接起解释器也可以，但要绕开 uv 的自动同步：

```bash
uv run --no-sync main.py
./.venv/bin/python main.py           # Windows 为 .venv\Scripts\python.exe main.py
```

`uv run` 会先按 `uv.lock` 同步环境，而默认依赖组是 `cpu`。装过 GPU 组的机器上，这一步会把 `torch` 换回 CPU 构建，所以这里要加 `--no-sync`。

服务默认监听 `0.0.0.0:8000`。本机可以打开 `http://127.0.0.1:8000/docs` 查看接口文档。语音合成要在网页里直接听，打开 `http://127.0.0.1:8000/tts/play`，`/docs` 里放不出声音，原因见第 11 节。

![本地接口文档](../../docs/pictures/本地网页访问.png)

服务使用两个本地文件：

- `file_db.sqlite`：文件元信息。
- `upload_files/`：上传的文件。

### MCP 网关

deploy 脚本默认还会起一个 MCP 网关，端口 `8765`。板端填这个地址：

```text
PKG_AGENT_MCP_SERVER_URL=http://PC-IP:8765/mcp
```

网关把 PC 上的工具收成一个 MCP 端点：Windows-MCP 的桌面操作、网关自带的文件工具，还有 `mcp/tools/user_tools/` 里的用户工具。工具回包里的图片和音频由网关存到文件服务器，回给板端的只是一句带下载地址的说明，板端的 32 KB 缓冲才装得下。

网关的代码在 `mcp/` 目录，细节见 `mcp/README.md`。只想要文件服务器时加 `--no-mcp`，换端口用 `--mcp-port 8766`。用 `--start` 点名组件时，`mcp` 不在名单里网关就不起。

## 4. 接口

| 接口 | 方法 | 说明 |
|---|---|---|
| `/upload` | POST | 上传文件，form 字段名为 `file`，返回 `file_id` 等信息 |
| `/download/{file_id}` | GET | 按 `file_id` 下载文件 |
| `/files/{file_id}` | GET | 查询单个文件的元信息 |
| `/files` | GET | 列出全部文件 |
| `/files/{file_id}` | DELETE | 删除文件 |
| `/stt` | POST | 语音转文本，form 字段名为 `file`，返回 `{"code":0,"data":{"text":"..."}}` |
| `/tts/speakers` | GET | 列出已装配的音色与可用语种码 |
| `/tts` | POST | 文本转语音，直接返回 `audio/wav` 字节。请求带 `"stream":true` 时改返回 `[4 字节小端长度][完整 WAV]` 的帧序列，见 11 节的流式分块。未装配 GPT-SoVITS 时返回 503，其余接口不受影响 |
| `/tts` | GET | 同样的合成，参数走 query，`GET /tts?text=你好` 本身就是一个音频 URL |
| `/tts/play` | GET | 浏览器试听页，带 `audio` 播放器。`/docs` 里播不出声音，试听要走这条 |
| `/tts/progress` | GET | 最近一次合成的进度，返回 `{"code":0,"data":{"stage":"...","index":N,"total":N,"audioSeconds":X,"elapsed":X,"message":"..."}}` |

上传接口返回示例：

```json
{
  "code": 0,
  "msg": "upload ok",
  "data": {
    "file_id": "0b0f0c7e-...",
    "original_name": "photo.jpg",
    "store_path": "./upload_files/0b0f0c7e-....jpg",
    "file_size": 12345,
    "upload_time": "2026-10-20 10:00:00"
  }
}
```

浏览器直接访问时，下载地址是：

```text
{服务根地址}/download/{file_id}
```

本地调试时为 `http://127.0.0.1:8000/download/{file_id}`。

Agent 侧的 `PKG_AGENT_WEB_SERVER_URL` 要填到端口为止，例如 `http://127.0.0.1:8000`。代码会自己拼 `/upload`、`/download/{file_id}`、`/stt`、`/tts`，不要在宏里带路径。内网穿透后把地址换成公网地址。

这两个地址的选项在固件的 `External Server` 菜单里。菜单还多一个来源选择：手动填死，或者启动时向管理控制台问一次。选自动模式时，上面填的地址是控制台连不上时的回退值，端口改了不用重新烧固件。控制台那边在运维页的「板端地址」填板子够得着的主机名。

板端和模型服务取的不是同一个地址。板端上传走 `PKG_AGENT_WEB_SERVER_URL`，模型下载走板端回传的下载 URL，两条链路共用同一个基址。板端填本机地址时，只有板端这一侧通，模型服务抓不到文件。

## 5. 语音转写

`/stt` 使用 openai-whisper 在本地识别，不调用外部服务。

上传的 wav 不限采样率：`main.py:250-257` 先下混成单声道，非 16 kHz 时用 resampy 重采样后再喂模型。whisper 只在传文件路径时才自己重采样（`whisper/audio.py:25-40` 的 `load_audio` 要 ffmpeg CLI），传 ndarray 必须是 16 kHz（`audio.py:122-123`），否则 32 kHz 音频会被当成两倍速，识别结果退化成乱码。

服务启动时会在后台加载模型。首次运行会下载模型，可能耗时数十秒；加载完成后，后续请求直接复用。

可用环境变量调整：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `STT_MODEL` | `tiny` | whisper 模型，可选 `tiny`、`base`、`small` |
| `STT_LANG` | `zh` | 识别语言 |

Windows PowerShell 示例：

```powershell
$env:STT_MODEL = "base"
$env:STT_LANG = "zh"
uv run --no-sync main.py
```

Agent 侧配置：

1. 打开多模态开关 `PKG_AGENT_MULTIMODAL_ENABLE`。
2. 打开 `PKG_AGENT_STT_ENABLE`。
3. 把 `PKG_AGENT_WEB_SERVER_URL` 填为 `http://<服务器地址>:8000`，板端会拼出 `/stt`。

录音格式固定为 16000 Hz、双声道、16 位 WAV。服务端读取后转成单声道再识别。

STT 失败时，Agent 不会上传音频，而是给模型一条“语音消息转写失败，请再说一次”的提示文本。排查时先看服务端是否返回 500，再确认模型是否已下载完成。

## 6. 内网穿透

开发板和 QEMU 通常访问不到宿主机的 `127.0.0.1`。需要把本机 8000 端口映射到公网。

仓库自带飞鸽内网穿透的 Windows 启动器：`resource/飞鸽启动器-0.4.exe`。也可以使用其他穿透工具。

通道类型优先选付费通道，带宽和稳定性更好，大文件上传不容易超时。

![选择穿透通道](../../docs/pictures/飞鸽内网穿透建议通道.png)

通道配置：

| 配置项 | 填写 |
|---|---|
| 本地地址端口 | `127.0.0.1:8000`，与文件服务器监听端口一致 |
| 前置域名 | 自定义前缀 |
| 证书 | 系统默认证书 |

![通道参数](../../docs/pictures/飞鸽内网穿透通道配置.png)

穿透成功后会得到 HTTP 与 HTTPS 两个地址。

HTTP 地址：

![HTTP 访问地址](../../docs/pictures/飞鸽内网穿透http访问.png)

HTTPS 地址：

![HTTPS 访问地址](../../docs/pictures/飞鸽内网穿透https服务.png)

### 上传地址使用 HTTP

Agent 的 `PKG_AGENT_WEB_SERVER_URL` 建议填 HTTP 地址，例如：

```text
http://xxx.fgnwct.com
```

实测 RT-Thread 通过 HTTPS 上传会遇到问题，表现为上传失败或返回 20040。`/stt` 同理，建议使用 HTTP。

板端自己拼出的下载地址要给大模型用，必须是公网可访问的地址。模型服务访问不到内网地址和 `localhost`，使用 HTTPS 时证书也要被公网信任。

## 7. 配置示例

Agent 的多模态配置：

```text
PKG_AGENT_WEB_SERVER_URL = http://xxx.fgnwct.com
```

板端按这个地址拼出四个接口：

```text
上传     <地址>/upload
下载     <地址>/download/<file_id>
语音识别 <地址>/stt
语音合成 <地址>/tts
```

地址只填到端口，不要带路径，也不要带 `file_id`。

menuconfig 中的填写位置：

![文件服务器地址配置](../../docs/pictures/文件上传服务器menuconfig设置.png)

## 8. 验证

按顺序检查：

1. `uv run --no-sync main.py` 正常运行，浏览器能打开 `127.0.0.1:8000/docs`。
2. 穿透通道在线，HTTP 地址在浏览器中可访问。
3. `PKG_AGENT_WEB_SERVER_URL` 使用 HTTP 公网地址。
4. 开发板网络正常；使用 HTTPS 下载时，系统时间已同步。
5. 上传后复制 `file_id` 拼出的下载 URL，在无痕窗口能直接访问。
6. 下载 URL 中的 `file_id` 是完整的 36 位 UUID，没有截断。
7. `curl -I <下载URL>` 返回 200，`Content-Type` 为图片或音频类型。

## 9. 多模态聊天示例

1. 通道选择 `PKG_AGENT_DEBUG_CHANNEL`。
   ![](../../docs/pictures/启用PKG_AGENT_DEBUG_CHANNEL.png)

2. 发送一张图片。
   ![](../../docs/pictures/多模态测试照片.jpg)

3. 模型回复。
   ![](../../docs/pictures/多模态照片demo.jpg)

## 10. 常见问题

| 现象 | 排查 |
|---|---|
| `uv sync` 超时 | 配置国内镜像源后重试 |
| `no kernel image is available for execution on the device` | torch 的 CUDA 构建与显卡算力不匹配：Blackwell 误装了 `gpu-cu126`，或老卡装了 `gpu-cu130`。重跑 `bash deploy.sh` / `deploy.bat` 让脚本重新判定 |
| GPU 环境莫名变回 CPU | 跑过不带 `--group` 的 `uv sync`，或不带 `--no-sync` 的 `uv run`。默认组是 `cpu`，重跑 `bash deploy.sh` / `deploy.bat`，脚本按探测结果装回对应后端 |
| 板子上传返回 20040 | 确认上传地址是 HTTP；确认穿透地址公网可访问；确认 `file_id` 完整；确认下载响应的 `Content-Type` 是图片类型 |
| 设备连不上服务器 | 检查穿透通道的本地端口是否为 `127.0.0.1:8000`，检查防火墙 |
| 上传成功，但模型下载不了图片 | 下载 URL 必须公网可达；HTTPS 证书需被公网信任 |
| 下载返回 404 | `file_id` 不完整或已被删除，可用 `/files` 核对 |
| `/stt` 返回 500 | 查看服务端日志，确认 whisper 模型已下载、音频文件可读 |
| `/tts` 或 `/stt` 返回 404 | 启动时没选这个组件。服务只注册选中的组件的端点，用 `--start` 把 `tts` 或 `stt` 点上 |
| 装依赖报「拒绝访问」（os error 5） | Windows 上文件服务器在跑，torch 的 DLL 被占着。先点停止再装依赖 |
| 首次语音识别很慢 | 首次请求会加载模型，等待时间取决于模型大小与磁盘速度 |
| `/stt` 请求超时 | Agent 侧连接后等待 120 s；模型过大或机器太慢时，换 `tiny` 模型 |
| `/tts` 返回 503 | 没装 GPT-SoVITS 推理链，或权重没落到 `GPT_weights_v2Pro/`。跑 `bash deploy.sh --tts` / `deploy.bat --tts`，再用 `python fetch_tts.py check` 核对 |
| 第一次合成等了两三分钟 | 服务端没预热，首次请求要建管线并即时编译。设 `TTS_AUTOLOAD=1`，服务启动时就在后台把模型拉起来 |
| `/tts` 之前能用，跑过一次不带 `--tts` 的 deploy 后返回 503 | 依赖被 `uv sync` 剪掉了。重跑 `deploy.bat --tts`，脚本会把 125 个包从缓存装回来；新版脚本会先探测 `.venv`，已装过的环境不带 `--tts` 也不会剪 |
| 手工跑过 `uv sync` 之后环境不对 | 这条命令按组剪包，`tts` 组、GPU 后端、`eunjeon` 三处都会退回去。别再用它部署，重跑 `bash deploy.sh --tts` / `deploy.bat --tts` 修回来，后果清单见第 2 节 |
| 合成韩语报 `TypeError: exceptions must derive from BaseException` | `eunjeon` 没装，或 `setuptools` 被 `uv sync` 退回 84.0.0，真正的报错是它删掉的 `pkg_resources`。补包命令见第 11 节 |

## 11. 可选：GPT-SoVITS 语音合成

`main.py` 里的 `/tts` 是可选功能。推理代码已经抽进仓库，放在 `third_party/gpt-sovits-infer/`，只留合成一条链。预训练模型、大词表、FFmpeg 不入库，装配前该接口返回 503，其余接口照常工作。音色是自家微调的 `da_fei_yv`（v2Pro）与 `da_fei_yv_plus`（v2ProPlus）：参考音频、`manifest.json` 与音色注册表在仓库里，四份权重单个 129 MiB～165 MiB，放在 ModelScope 的 `Caoxiansong/da_fei_yv`，由 `voice` 步骤拉回并核对 SHA256。

装配这条链有三个动作：装环境、拉第三方资产和逐项自检。脚本一次跑完：

```bash
bash deploy.sh --tts             # Linux/macOS：装配 + 拉取 + 自检 + 启动
```

```bat
deploy.bat --tts                 :: Windows：同样的流程
```

参数表在第 2 节，两个脚本共用。这里只补 `--tts` 那条链要防的两处剪包，脚本都已经处理过。

第一处是 `tts` 组，剪包规则见第 2 节。裸跑 `deploy.sh` / `deploy.bat` 只选后端组，脚本在同步前查一次 `librosa`，查到就按 `--tts` 处理，打印一行 `tts group already installed, keeping it`。确实要把推理链卸掉时，手工跑 `uv sync`。

第二处是 `eunjeon`。韩语那条路要用它，它又依赖 `setuptools<81` 才有的 `pkg_resources`。这两个包是 `uv pip install` 补进去的，`eunjeon` 在锁文件里根本没有，`setuptools` 锁在 84.0.0，每次同步都会退回这两个状态，所以脚本拉完资产再查一次元数据，缺了就把它们补回去。

想手动分步跑时，顺序是这样：

```bash
bash deploy.sh --tts --check                    # 装配 + 拉取 + 自检，不起服务
python fetch_tts.py dicts voice assets          # 都要联网，assets 要几 GB 空间
python fetch_tts.py check                       # 逐项自检，缺什么一眼看到
```

![TTS 部署流程](../../docs/diagrams/tts-deploy-flow.png)

draw.io 源文件：[tts-deploy-flow.drawio](../../docs/diagrams/tts-deploy-flow.drawio)

`fetch_tts.py` 做的事，全部幂等（已存在即跳过）：

- `dicts`：取 8 个上游放在 git 里、单个就超过 500 KB 的数据文件（`text/cmudict.rep`、`text/engdict_cache.pickle`、`text/ja_userdic/userdict.csv`、`Accel/vendor/cuda/bin/cudart64_12.dll` 等），共 35.3 MB。按钉住的 commit 从 `raw.githubusercontent.com` 下载，逐个核对 SHA256；本地已经存在且校验通过就跳过，校验失败则删掉重下。
- `voice`：从 ModelScope 的 `Caoxiansong/da_fei_yv` 拉四份权重，`da_fei_yv` 的 s1/s2 与 `da_fei_yv_plus` 的 s1/s2，落到各自的版本目录，逐个核对 SHA256，期望值与 `refs/<音色>/manifest.json` 一致。直链匿名可下，不需要 token；curl 带 `-C -`，断点续传。
- `assets`：从 ModelScope 的 `XXXXRT/GPT-SoVITS-Pretrained` 解出 `pretrained_models.zip`（hubert、bert 等）到 `third_party/gpt-sovits-infer/GPT_SoVITS/`、`G2PWModel.zip` 到 `GPT_SoVITS/text/`、`nltk_data.zip` 到 `runtime/`，解压目录沿用上游 `install.sh:262`、`:275`、`:362` 的目标；再从 fastText 官方下 `lid.176.bin` 并校验 MD5；Windows 上额外解一份 FFmpeg 共享库到 `runtime/`，因为 venv 里 torchaudio 2.11 的 `load()` 已转发 TorchCodec，没有外部 FFmpeg 就报 `Could not find module libtorchcodec_coreN.dll`。

换源与代理：`--source hf` / `--source hf-mirror` 切 zip 来源，`--mirror` 让 GitHub 走 gh-proxy。国内直连 BtbN 的 FFmpeg Release 会报 curl 退出码 35（SSL 连接被断），所以 Windows 上这一步基本要配 `--mirror`；gh-proxy 实测只有 80 KB/s ~ 400 KB/s，88 MB 的 zip 得等几分钟。`lid.176.bin` 已随 `pretrained_models.zip` 落地，脚本仍按 MD5 复核一遍。

取完之后 `third_party/gpt-sovits-infer/` 是 6.1 GB：`GPT_SoVITS/pretrained_models/` 4.6 GB、`GPT_SoVITS/text/G2PWModel/` 608 MB、`runtime/` 228 MB、8 个词表 35.3 MB、四份权重 590 MiB（148 MiB、129 MiB、148 MiB、165 MiB）、参考音频 0.3 MB。入库的代码和配置 169 个文件、1.9 MB。

推理子集怎么选出来的，记在 `third_party/gpt-sovits-infer/README.md`：判据是一次真实中文合成在 `sys.modules` 里留下的 97 个上游 .py，再按目录规则放宽到其它语种的文本模块和各个 `__init__.py`；训练、导出、WebUI、ASR、人声分离全部丢掉。落库 169 个文件，其中 157 个是上游推理文件，另加上游 MIT `LICENSE`、本说明、两份音色注册表和每个音色四个自有文件（参考音频、`manifest.json`、`prompt.txt`、`tts_infer.custom.yaml`）。升级或重抽用同目录的 `extract_infer.py`，取舍清单是里面的 `DROP_PREFIX`。

装配自检，不经过 HTTP。在 `file_server` 目录里用带 tts 组的 venv 起 Python，把下面这段从标准输入喂进去（`python -` 与交互式解释器都把当前目录放进 `sys.path`，写成 .py 文件用绝对路径跑则 `import main` 会失败）：

```bash
./.venv/Scripts/python.exe - <<'PY'      # Linux/macOS 换成 ./.venv/bin/python
from pathlib import Path
import main

out = Path("out.wav").resolve()          # 先转绝对路径，_tts_synth_wav 会切走工作目录
speakers = main._tts_speakers()
name = main._tts_default(speakers)
sr, wav = main._tts_synth_wav(speakers[name], "今天天气不错，我们出去走走吧。", "zh", 1.0, 16000, 2)
out.write_bytes(wav)                     # 2 声道、16 kHz，板侧播放用的就是这档
print(name, sr, len(wav))
PY
```

实测输出 `da_fei_yv 16000 253484`，落盘的 `out.wav` 是 2 声道、16 kHz、3.96 s。`_tts_synth_wav` 内部把工作目录切到 `third_party/gpt-sovits-infer/`，之后再写相对路径就落到别处去了。上游的 `inference_cli.py`、`inference_webui.py`、`api_v2.py` 没有抽进子集，别照着它们写调用。

七个语种各合成一句，都出音频。自家 `da_fei_yv` 音色在同一台机器上的实测（16 kHz 双声道，峰值是 int16 幅度）：

| `language` | 字节 | 时长 | 峰值 | 削顶样本 |
|---|---|---|---|---|
| `zh` | 253 484 | 3.96 s | 21 857 | 0 |
| `all_zh` | 151 084 | 2.36 s | 18 486 | 0 |
| `en` | 215 084 | 3.36 s | 26 926 | 0 |
| `ja` | 201 004 | 3.14 s | 26 620 | 0 |
| `yue` | 171 564 | 2.68 s | 24 633 | 0 |
| `ko` | 126 764 | 1.98 s | 26 544 | 0 |
| `auto` | 302 124 | 4.72 s | 24 917 | 0 |

`ko` 要先补两个包，缺任何一个，建管线时都报 `TypeError: exceptions must derive from BaseException`：

```bash
uv pip install --python ./.venv/Scripts/python.exe eunjeon "setuptools<81"
```

这条报错不是抽取造成的，来自第三方包 `g2pk2`。`g2pk2/g2pk2.py:90` 的 Windows 分支把加载 eunjeon 的任何异常塞进 `raise print(...)`，`print()` 返回 `None`，于是真正的原因（`eunjeon/__init__.py:11` 要 `import pkg_resources`，setuptools 84 已删除该模块）被盖掉了。`pyproject.toml` 的 `tts` 组声明了 `g2pk2`、`ko-pron` 和 `python-mecab-ko ; sys_platform != 'win32'`，Windows 上的 Korean MeCab 只能靠 eunjeon，得手动补。补齐后「오늘 날씨가 좋습니다.」走 `/tts` 返回 200、126 764 字节、1.98 s，峰值 26 544，无削顶。同一句在进程内自检时长会差几十毫秒，是 GPT 采样本身的抖动。

`ja` 第一次合成实测 253.9 s（解 pyopenjtalk 词典），词典落盘之后同语种稳定在 2～3 s。多出来的是解词典的开销，不是推理变慢。

### 做成服务

装配完成后，`main.py` 里的 `/tts` 直接可用。它在同一进程内调 `GPT_SoVITS/TTS_infer_pack/TTS.py` 的 `TTS_Config` 与 `TTS`，不另起 api_v2 服务，也不碰 gradio。`_tts_activate()` 复用上游自己的 `tools/portable_runtime.activate()`。它会把工作目录切到 `third_party/gpt-sovits-infer/`，所以 `main.py` 开头先把 `UPLOAD_DIR`、`DB_PATH` 和上传记录里的相对路径统一按 `__file__` 展开成绝对路径。

音色靠扫描注册。权重按训练版本分目录，`_TTS_WEIGHT_DIRS` 列出版本到目录的对应关系：`v2` 用 `GPT_weights_v2/` 与 `SoVITS_weights_v2/`，`v2Pro` 用 `GPT_weights_v2Pro/` 与 `SoVITS_weights_v2Pro/`，`v2ProPlus` 用 `GPT_weights_v2ProPlus/` 与 `SoVITS_weights_v2ProPlus/`。同一版本目录内的 `.ckpt` 与 `.pth` 按权重名前缀配对，`da_fei_yv-e40.ckpt` 加 `da_fei_yv_e6_s162.pth` 得到音色 `da_fei_yv`，目录名同时给出这个音色的推理版本。再要求 `tts_speakers/da_fei_yv.json` 提供参考音频。仓库里带的注册表内容如下：

```json
{
  "ref_audio": "refs/da_fei_yv/dwg_0002.wav",
  "ref_text": "refs/da_fei_yv/prompt.txt",
  "ref_language": "zh"
}
```

两个路径相对 `third_party/gpt-sovits-infer/`。再加一个音色，放好两套同名权重、仿照这份 JSON 写一份注册表就行；权重若来自社区，按 `.gitignore` 挡住不入库。

### 导入本地导出的权重

自己微调导出的权重按下面的布局摆放：每个音色两套权重、`refs/<音色>/` 四件留档、`tts_speakers/<音色>.json` 一份注册表。仓库里带的 `da_fei_yv` 就是这么放的，导出目录里的原始文件名是 `dafeiyu2_v2Pro-e40.ckpt`、`dafeiyu2_v2Pro_e6_s162.pth` 和 `refs/dwg_0002.wav`，入库时统一换成 `da_fei_yv` 前缀。原名记在 `manifest.json` 每条的 `source` 和顶层 `renamed_from`，`file` 一律写仓库里的路径：

```text
third_party/gpt-sovits-infer/
├── GPT_weights_v2Pro/da_fei_yv-e40.ckpt              fetch_tts.py voice 从 ModelScope 拉
├── SoVITS_weights_v2Pro/da_fei_yv_e6_s162.pth       同上
├── GPT_weights_v2ProPlus/da_fei_yv_plus-e40.ckpt    同上，在同一个 ModelScope 仓库里
├── SoVITS_weights_v2ProPlus/da_fei_yv_plus_e8_s216.pth  同上
├── refs/da_fei_yv/dwg_0002.wav                       入库，参考音频
├── refs/da_fei_yv/manifest.json                      入库，导出时的测量值留档
├── refs/da_fei_yv/prompt.txt                         入库，从 manifest 的 prompt_text 落一份
├── refs/da_fei_yv/tts_infer.custom.yaml              入库，导出时那份推理配置
├── refs/da_fei_yv_plus/                              同样四件，音色名换成 da_fei_yv_plus
└── tts_speakers/da_fei_yv.json                       入库，只有三个字段
```

`tts_speakers/da_fei_yv_plus.json` 也在库里，只有参考音频路径、转写文本路径和语种三个字段。

四份权重合计 590 MiB，超过 GitHub 的单文件上限，仓库里不带：`fetch_tts.py voice` 从 ModelScope 一次拉四个，SHA256 对得上才留下。换音色时照这个布局再放一套即可，目录名决定推理版本。

v2ProPlus 那一套音色名 `da_fei_yv_plus`，`refs/da_fei_yv_plus/manifest.json` 记了哈希与训练配方，2026-10-31 发布到同一个 ModelScope 仓库，`fetch_tts.py voice` 把两档四个文件一起拉下来，新装的机器 `GET /tts/speakers` 列两个音色。不带 `speaker` 时的默认音色走 `_tts_default()`：`TTS_SPEAKER` 优先，其次是 `_TTS_PREFERRED_SPEAKER` 指的 `da_fei_yv_plus`，这一档不在音色表里才退回排序首个。选 v2ProPlus 最初是想试逐 token 流式 `gpt_streaming`（`TTS_infer_pack/streaming.py:12` 只放 v2ProPlus/v5turbo/v5dev），2080 Ti 上实测是负收益：同一份权重 `streaming_mode` 首帧 0.62～0.93 s、RTF 0.19～0.24，换 `gpt_streaming` 是 0.83～1.59 s、RTF 0.19～0.38，且它按 CUDA graph 静态形状解码，预热完的第一条真实回复要补交一次图捕获，首帧 16.25 s、启动预热从 21.3 s 涨到 165.4 s。因此 `_tts_stream_frames` 两个版本都只走 `streaming_mode`，v2ProPlus 剩下的差别只有听感。同一进程里换音色要重载两套权重，实测那一次请求首帧 9.87 s（v2ProPlus→v2Pro），之后的请求才回到 0.6 s 上下；2026-10-31 又测了一轮，切过去首帧 10.00 s、切回来 5.31 s，同一音色连发三条回到 1.14～1.39 s。

文件名要能配成对：`.ckpt` 去掉 `-e\d+` 之后的前缀，和 `.pth` 去掉 `_e\d+_s\d+` 之后的前缀要一致，两边都得到 `da_fei_yv`。权重名里带不带版本都行，版本由所在目录决定。

`main.py` 不需要为 v2Pro 或 v2ProPlus 改推理调用。建管线时把目录给出的版本传给 `TTS_Config`（`_tts_pipeline()`），换音色走 `init_t2s_weights` 与 `init_vits_weights`。上游读 s2 权重头两字节判版本（`process_ckpt.py:77` 的 `head2version`，`b"05"` 是 v2Pro、`b"06"` 是 v2ProPlus），Pro 系列顺手加载 ERes2NetV2 说话人模型（`TTS.py:584`），`TTS.py:646` 置 `is_v2pro`，推理时据此把参考音频重采样到 16 kHz（`TTS.py:909`）并取说话人向量（`TTS.py:1411`）。

一条管线能在 v2 与 v2Pro 之间来回切。实测同一进程按 v2 → v2Pro → v2Pro → v2 连做四次合成，原生采样率都是 32 000 Hz：首次含建管线 105.8 s 出 6.48 s 音频，切到 v2Pro 11.3 s 出 6.44 s，换一句 2.0 s 出 4.16 s，切回 v2 3.5 s 出 3.00 s。加载 VITS 权重时打印的 `missing_keys=['enc_q.*']` 不是导入出错，推理权重本来不带 `enc_q`，上游用 `strict=False` 加载。

走 `/tts` 整条返回（`stream=false`）的端到端实测：请求 `{"speaker":"da_fei_yv","language":"all_zh","sample_rate":16000,"channels":2}`，返回 200、294 444 字节，响应头 `x-speaker: da_fei_yv`、`x-sample-rate: 16000`；产物 16 kHz 双声道、4.60 s、峰值 31031、无削顶，`/stt` 回读「你好 我是大飛魚今天天氣不錯」。用 manifest 里那句 23 字自测文本合成得 6.44 s，落在导出记录写的 6.12～7.72 s 抖动带内；导出侧固定 seed 43，`/tts` 这条链路没传 seed，时长每次会抖。

有两个前提要满足。s2 权重如果前两个字节不是 `PK`，必须经 `load_sovits_new`（`process_ckpt.py:141`）补上 `b"PK"` 再 `torch.load`，`init_vits_weights` 已经这么调了，别绕过它自己加载。v2Pro 与 v2ProPlus 都依赖 `GPT_SoVITS/pretrained_models/sv/pretrained_eres2netv2w24s4ep4.ckpt`（107 528 697 字节，`sv.py:6` 按相对路径找它），缺了 `init_sv_model()` 会在加载时抛文件不存在；这份由 `fetch_tts.py assets` 取，`fetch_tts.py check` 会报。

导出包里的 `manifest.json` 和 `tts_infer.custom.yaml` 建议一起放进 `refs/<音色名>/` 留档，训练参数、seed 和当时的时长都记在里面，验收时对着看。

`language` 传 `TTS_infer_pack` 的引擎语种码：`all_zh`、`zh`、`en`、`all_ja`、`ja`、`all_yue`、`yue`、`all_ko`、`ko`、`auto`、`auto_yue`。中文名也收，`_TTS_LANG_ALIAS` 先换算，表抄上游 WebUI 的 `dict_language_v2`（`inference_webui.py` 未抽进子集，对照时用上游 commit）。只能用引擎码这条限制来自 `TTS.py:1251` 的 `assert text_lang in self.configs.languages`，而 `TTS_Config.languages` 就是英文码表（`TTS.py:359`）。传成 WebUI 的显示名时，断言没有消息文本，接口只会回一条 `tts failed: AssertionError:`，看不出原因。

请求体字段：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `text` | 必填 | 待合成文本，空串返回 400；表情符号和不可念的符号先由 `_tts_sanitize()` 处理，净化后为空返回 400 |
| `speaker` | `TTS_SPEAKER`，未设则取 `da_fei_yv_plus`，该音色不存在时取排序首个 | 名字不在表里返回 404 |
| `language` | `zh` | 中英混合。取值见上一段 |
| `speed` | `1.0` | 语速倍率 |
| `sample_rate` | 不重采样 | 板侧播放填 `16000` |
| `channels` | `1` | 填 `2` 时把单声道复制成左右两份；只接受 1 和 2，其他值返回 400 |
| `stream` | `false` | 填 `true` 时按帧流式返回，见下面的「流式分块」 |
| `block_seconds` | `1.5` | 一帧攒多少秒音频，只接受 0.5～5.0，越界返回 400 |

成功时响应体就是 WAV 字节，`Content-Type: audio/wav`，另带 `X-Sample-Rate` 与 `X-Speaker` 两个响应头。`TTS.run()` 交回来的是 int16 PCM（`TTS.py:1814`、`TTS.py:1822` 先乘 32768 再 `astype(np.int16)`），`main.py` 里整条路径在 `_tts_synth_wav()` 除一次 32768 归到 [-1, 1]，流式路径在 `_TtsBlocker` 除，之后才重采样、交给 soundfile 写 PCM_16。少了这一步，重采样分支拿到的是上万量级的浮点数，soundfile 全部削顶，产出的音频时长正常，内容是方波噪声。

同一句文本的对照实测：32 kHz 原生音频 `/stt` 回读为「這是第一次請求 模型又現場加載」，未归一化的 16 kHz 回读成一片乱码；归一化之后 16 kHz 回读为「这是第一次请求 模型又现场加载」，峰值 0.817，削波样本占比 0。

下面两张表是 `stream=false` 整条返回的实测，音色 `da_fei_yv`（RTX 2080 Ti、CUDA 12.6）：

| 场景 | 耗时或占用 |
|---|---|
| 模型预热 | 约 60 s，显存增加 1.4 GB，进程常驻内存 3.7 GB |
| 预热后的首次合成 | 约 40 s，含 G2PW 与 numba 的即时编译 |
| 稳定后合成 4.5 s 音频 | 3.5 s |
| 未开 `TTS_AUTOLOAD` 的首个请求 | 99.5 s，加载和首帧都压在这次请求上 |

换成自家的 `da_fei_yv` 音色后：进程内第一次合成 47.3 s（含建管线、G2PW 与 numba 即时编译），之后每句 2～3 s；HTTP 首次请求 61.3 s，管线在服务进程里现建，之后每句同样 2～3 s。v2 与 v2Pro 之间换音色不重建管线，只重载两套权重，实测 11.3 s（含换权重与一次合成）。

推理全程由 `_TTS_LOCK` 保护，同一时刻只跑一帧，多请求排队等待。请求处理放在 `asyncio.to_thread`，合成期间 `/upload`、`/download`、`/stt` 照常响应。CPU 推理没在这台机器上计时，`TTS_DEVICE=cpu` 的代价请按你机器自测。

| 变量 | 默认值 | 说明 |
|---|---|---|
| `TTS_AUTOLOAD` | `0` | 填 `1` 时启动即在后台线程预热模型，否则首个请求自己承担加载 |
| `TTS_SPEAKER` | 未设时取 `da_fei_yv_plus` | 默认音色。`deploy.bat --speaker NAME` 与 `deploy.sh --speaker NAME` 把它写进启动环境 |
| `TTS_DEVICE` | 有 CUDA 用 `cuda`，否则 `cpu` | 填 `cpu` 可在无显卡机器上跑 |
| `TTS_IS_HALF` | `cuda` 时 `1`，否则 `0` | 半精度开关 |

curl 示例。含中文的请求体不要拼在命令行里，Windows 的 shell 会改掉字节，FastAPI 回 `There was an error parsing the body`；写成文件再传：

```bash
printf '{"text":"今天天气不错。","language":"all_zh","sample_rate":16000}' > req.json
curl -s localhost:8000/tts/speakers
curl -s -X POST -H "Content-Type: application/json" --data @req.json localhost:8000/tts -o out.wav
curl -sG localhost:8000/tts --data-urlencode "text=今天天气不错" --data "sample_rate=16000" -o out.wav
printf '{"text":"今天天气不错。","sample_rate":16000,"channels":1,"stream":true}' > req_stream.json
curl -s -N -X POST -H "Content-Type: application/json" --data @req_stream.json localhost:8000/tts -o frames.bin
```

`stream` 只在 POST 体里有，GET `/tts` 一律整条返回，这样它本身才是一个可播放的音频 URL。`frames.bin` 不是 WAV，要按 `[4 字节小端长度]` 一帧一帧切开，每段自己带 WAV 头，切开就能播。

`/tts` 有 POST 与 GET 两条，合成逻辑同在 `main.py` 的 `_tts_handle()`：板侧走 POST（webclient 发 JSON 方便），人要听就走 GET。`/docs` 里点 `Try it out` 播不出声音，有两层原因：Swagger UI 对二进制响应只给下载链接，没有播放器；`Response` 返回值不带 `responses=` 声明时，OpenAPI 又把 200 写成 `application/json`，Swagger 拿 JSON 去解析 WAV 字节，连下载链接都不出现。`_TTS_WAV_RESPONSES` 把响应声明成 `audio/wav` + `binary`，`GET /tts/play` 那张单页再挂上 `audio` 标签，浏览器里就能直接试听和下载。

板侧已经接上：`src/utils/tts.c` 的 `msh_agent_tts` 走整条返回，回复自动播报走 `stream=true`，两条都带 `sample_rate:16000` 和 `channels:1`。

`channels` 取 1 还是 2 取决于板上 I2S 驱动怎么处理声道，换板子要重新验一次。本板（Infineon PSE84）的 `libraries/HAL_Drivers/drv_i2s.c` 里有个 `convert_mono_to_stereo()`，把送进来的每个采样复制成左右两个 PCM 槽，一个 2048 字节的块按 64 ms 播完，等于 32,000 B/s，正好是 16 kHz 单声道。喂双声道会被当成单声道再翻一倍，时长翻倍、音调低八度，所以本板填 1。早期在 QEMU vexpress 上得到的结论相反：那边的 pl041 模型（`hw/audio/pl041.c`）每次出帧固定取两个采样，同一句「你好，我是大肥鱼，今天天气不错」的单声道文件 4.40 s 内容 2.20 s 播完，`/stt` 回读是乱码；换成立体声后 5.24 s 内容捕获 5.29 s，回读为「你好 我是大肥鱼今天天气不错」。

回复文本里的表情符号要单独处理。上游 `TextPreprocessor.pre_seg_text` 有一句 `print(text)`，中文 Windows 的 stdout 默认 GBK，遇到 😸 这类字符直接抛 `UnicodeEncodeError`，`/tts` 回 500，看着像推理坏了。`main.py` 开头把 stdout/stderr 重设成 UTF-8，`_tts_handle()` 再剥掉非 BMP 平面（`U+10000` 以上）的表情符号。实测三种输入：中英混排带 😸 返回 200、531 244 字节；纯表情返回 400「text 去掉表情符号和不可念的符号后为空」；`✅` 这类 BMP 符号（GBK 也编不了）返回 200。板侧遇到 500 只打印 `http resp code 500` 和 `reply stream request failed`，这一条回复不发声，播报线程接着处理下一条。

非 ASCII 的符号也要在入口换掉。回复里同时出现 `°C` 和 `—` 这类符号时，引擎的切句器把它们连同其他非 ASCII 字符按中日韩文处理，两侧又是拉丁字母，于是切出一个不含汉字的“中文”片段。`clean_text` 把它规范成空串，`TextPreprocessor.get_bert_feature` 拿着空的 `word2ph` 走到 `torch.cat([])` 抛 `ValueError`，整条 `/tts` 回 500。上游只在整个句子层面过滤纯符号（`TextPreprocessor.py:101`），句内片段没人管。`_tts_sanitize()` 先把 `°C`、`°F`、`°`、`≈`、`±`、`%`、`—`、`–` 换成英文单词，再把白名单以外的字符削成空格。白名单是 ASCII 加中日韩文字和中文标点：ASCII 走 en 路径，符号最多被丢掉，切不出空片段。实测一条 647 字节的传感器回复（含 `°C` 5 处、`—` 1 处、`≈` 1 处），净化前 500、0.12 s 返回；净化后 200，42 帧、62.79 s 音频。单测那句出问题的 `' fluctuation under 0.4 °C — nothing unusual 喵。'` 也是同一组结果：500 变 200。

### 合成进度

`GET /tts/progress` 返回最近一次合成的进度，运维页靠它画进度条。响应是 `{"code":0,"data":{...}}`，`data` 里的字段：

| 字段 | 说明 |
|---|---|
| `stage` | `idle`、`loading`、`running`、`done`、`error` |
| `index` | 已合成第几句 |
| `total` | 一共几句，按引擎自己的切句结果算 |
| `audioSeconds` | 已生成的音频秒数 |
| `elapsed` | 这一轮已经跑了多少秒 |
| `message` | 一句话说明，推理期间带已用时长 |

引擎的 `TTS.run()` 是一次性交回整段音频的，句子级的进度要等它返回才有。推理期间 `_TtsHeartbeat` 每秒刷一次进度，把已用时长写回 `elapsed` 和 `message`，这样页面能看出程序还活着。冷启动要装权重、编译 CUDA graph，本机实测整机要跑十分钟上下。

进度只记最近一次合成，服务重启后回到 `idle`。

### 流式分块

整条返回时，第一句要等全部合成完。同一接口 curl 实测：16 字的首字节 2.35 s、返回 189,484 字节（5.92 s 音频）；170 字的首字节 12.08 s、总耗时 16.21 s、返回 1,370,924 字节（42.84 s 音频）。合成时间约等于固定 0.8 s 加每秒语音 0.28 s，字数越多等得越久。

`stream=true` 把这条等待拆短。`_tts_stream_frames()` 用引擎的 `streaming_mode=True` 跑，`_TtsBlocker` 攒够 `block_seconds` 秒就发一帧，每帧自带完整 WAV 头，客户端收到就能播。引擎参数固定为 `min_chunk_length=16`、`overlap_length=2`：25 Hz 语义帧下 16 个 token 约 0.64 s 音频，再小 SOLA 拼接的痕迹会显眼。重采样按 overlap-save 处理，带上 512 样本尾巴再丢掉，否则块边界会有一声爆音。

响应头多一个 `X-TTS-Framing: u32le-wav`。帧格式是 `[4 字节小端长度][WAV]`，长度字段只算 WAV 本身。流关闭表示结束，没有单独的结束帧。

数据段补零到 2,048 字节的整数倍。板侧 `wavplayer` 每轮 `fread(buf, 2048, 1, fp)`，返回的是条目数，不满一条就判定文件结束，那半截一个样本也不写进声卡（`packages/wavplayer-latest/src/wavplayer.c:412`）。1.5 s 一帧的样本正好是 48,000 字节 = 23×2,048 + 896，尾巴 896 字节被丢掉，每个块边界少 28 ms 语音，连播多句时听着就是断句。补到 24×2,048 = 49,152 字节，一帧落盘 49,196 字节，多出来的 1,152 字节是 36 ms 静音，垫在块尾。`_tts_synth_wav()` 和 `_TtsBlocker._emit()` 共用 `_tts_pad_frames()`，整条返回的末段同样补齐。

引擎按句推理，某一句抛异常时前面的帧已经发出去了。`_tts_stream_frames()` 把整段推理循环包在 try 里，异常只打一条 `[tts] stream stopped after N blocks: ...`，然后照旧收尾，让 `StreamingResponse` 补上终止 chunk。不兜的话 uvicorn 直接掐断连接，板侧读下一帧头拿到 `-7`（`-WEBCLIENT_DISCONNECT`，`packages/webclient-v2.2.0/src/webclient.c:1330`）。socket 是在读完上一帧最后一个字节、去取下一个 chunk 长度时发现对端已经关闭的，那个返回值在 `webclient.c:1367` 被丢掉，所以前一帧仍然完整落盘，下一次读帧头才报错。2026-10-31 板上实测：14 帧、21 s 音频正常播完，日志是 `read frame header failed (-7)` 加 `stream broke after 14 blocks`，把“回答被截断”报成了“链路断了”。兜住之后的代价是板侧分不清正常说完和被截断，两种都读到帧头不足 4 字节为止，截断只留在服务端日志里。

`block_seconds` 扫过 0.5、1.0、1.5、2.5。0.5 s 只把起播提前 0.25 s，而帧间隔最长 0.97 s 已经超过一块的长度，播放会饿；1.5 s 时最长间隔 1.02 s～1.26 s，短于块长。板侧 `AGENT_TTS_BLOCK_SECONDS` 定 1.5。

两处一次性开销要提前吃掉。进程第一次推理 52 s～60 s，是前端模型和 CUDA 初始化，`TTS_AUTOLOAD=1` 的预热走完整条流式链路来承担它。预热的文本长度也是变量：用「预热。」两字热完，启动后第一条 23 字回复首帧 5.53 s；换成一句 31 字，同一句 1.37 s，往后 0.58 s～0.66 s。`cudnn.benchmark` 在服务里显式关掉，引擎在 `GPT_SoVITS/Accel/PyTorch/__init__.py:23` 把它打开，同一个新卷积形状都要现场搜一遍算法。开着时同一条「启动预热加首条真实回复」链路首帧 13.8 s，关掉 1.7 s，代价是稳态 RTF 从 0.249 升到 0.263。

关掉之后仍剩一个窗口：预热句和真实回复字数不同时，第一条回复要再付一次形状搜索。2026-10-31 两次重启后实测首帧 12.44 s 和 14.14 s，往后同一批 77 字、3 字、46 字的回复是 1.16 s、0.78 s、1.64 s。测延迟前先按目标回复长度发一两句废文本。

本机 GPU、不含公网隧道的稳态节奏：首帧 0.78 s～1.64 s，RTF 0.16～0.27，帧间隔最长 1.26 s。补零之后四条回复的帧数 3/13/2/9，数据段全是 2,048 的整数倍，起播和节奏没有变。经公网隧道发一句 33 字，5 帧共 221 680 字节、6.92 s 音频，客户端 2.53 s 收完，隧道没有成为瓶颈。

### 全新 clone 复刻

从空目录 clone 一份 `Agent-latest`，按上面三步装配，整条链能自己跑起来。2026-10-30 的实测记录：

| 步骤 | 结果 |
|---|---|
| `bash deploy.sh --tts` | torch 2.14.1+cu126，CUDA 可用，`.venv` 5.2 GB |
| `fetch_tts.py dicts` | 8 个词表逐个核对 SHA256 通过 |
| `fetch_tts.py voice` | 四个权重从 ModelScope 拉回，SHA256 对上 |
| `fetch_tts.py assets` | 解出 `pretrained_models.zip` 4.24 GB，`third_party` 合计 5.8 GB |
| `fetch_tts.py check` | `all third-party pieces present` |

起服务后合成一句「今天天气不错，我们出去走走吧。」：

| 请求 | 字节 | 耗时 | 产物 |
|---|---|---|---|
| 首次 | 268 844 | 203.3 s | 2 声道、16 kHz、4.20 s、峰值 29 870 |
| 第二次 | 268 844 | 2.62 s | 同上 |

首次那 203 s 里包含建管线、G2PW 与 numba 的即时编译，模型常驻后每句 2～3 s。两次返回的字节数相同，长度稳定。整个 clone 加 `.venv` 占 12 GB，其中 5.2 GB 是 Python 环境，5.8 GB 是推理资产。

2026-10-31 在开源发布副本里单跑了一遍 `fetch_tts.py voice`。那份仓库把包当普通目录入库，没有 `.venv`，也没有任何权重，等价于一次全新 clone。四个文件全部落地，SHA256 与脚本里的 `VOICE_WEIGHTS`、两份 manifest 都对得上，大小分别 155 313 376、134 946 419、155 313 376、172 765 363 字节，合计 590 MiB。之后 `fetch_tts.py check` 有六项音色探针转 `ok`（两档 s1/s2 加两份参考音频），剩下 15 项 `absent` 全归 `dicts` 与 `assets`，是预期结果。用 `_tts_speakers()` 的配对规则复核该目录，解析出 `da_fei_yv`（v2Pro）与 `da_fei_yv_plus`（v2ProPlus）两个音色。四份文件的头两字节：s1 都是 `PK\x03\x04` 的 torch zip，301 个条目、295 个张量；s2 分别是 `b"05"` 与 `b"06"`，即上游 `head2version` 里的 v2Pro 与 v2ProPlus。这一轮只验证取权重与音色解析，`assets` 要几 GB，没在发布副本重复跑。

### 重新部署与启动

已经装配好的环境再跑一遍脚本是幂等的，不重下资产。2026-10-30 的实测记录：

| 命令 | 结果 |
|---|---|
| `bash deploy.sh --tts --check` | `all third-party pieces present` |
| `deploy.bat --tts --check` | 同上，输出逐条一致 |
| `deploy.bat --tts --port 8001` | 起服务，`/tts/speakers` 返回 `da_fei_yv` |
| `deploy.sh --start-only --port 8002` | 5 s 内起来，`/tts/speakers` 同样返回 `da_fei_yv` |

`uv sync` 每次都会把 `eunjeon` 与 `setuptools` 剪回锁文件里的版本，`setuptools` 从 80.10.2 退回 84.0.0。脚本在拉完资产后查一次分发元数据，缺了就补回去，所以每次跑完环境都是对的。

合成一句「今天天气不错，我们出去走走吧。」，请求体 `{"language":"all_zh","sample_rate":16000,"channels":2}`：

| 请求 | 字节 | 耗时 | 产物 |
|---|---|---|---|
| 首次 | 258 604 | 81.1 s | 2 声道、16 kHz、4.04 s、峰值 29 660 |
| 第二次 | 240 684 | 1.91 s | 2 声道、16 kHz、3.76 s、峰值 29 589 |

首次那 81 s 是服务进程里现建管线，加上 G2PW 与 numba 的即时编译。管线建好后每句 2 s 上下。两次字节数不同，因为 `/tts` 这条链路没有传 seed，采样每次会抖，时长跟着变。

### 运行时链路

板端发出合成请求到喇叭出声，中间经过这几步：

![TTS 运行时链路](../../docs/diagrams/tts-runtime-flow.png)

draw.io 源文件：[tts-runtime-flow.drawio](../../docs/diagrams/tts-runtime-flow.drawio)

图里那条流式链路是板子打开 `PKG_AGENT_TTS_AUTO_PLAY` 之后的走法。板侧请求不带 `speaker`，服务端按 `TTS_SPEAKER` 取默认音色，没设时走 `_tts_default()` 的 `da_fei_yv_plus`，`GET /tts/speakers` 的 `default` 字段就是板子听到的那个。2026-10-31 经公网隧道用板侧原样的请求体复现过一次：33 字一句，2.53 s 收完 5 帧，共 6.92 s 音频，帧内 WAV 是 16 kHz 单声道 16 位。板侧收流与两槽乒乓的细节见 [framework.md 的文本转语音一节](../../docs/framework.md)。

## 12. 第三方与许可

`.gitignore` 只挡体积大或许可受限的第三方资产（`third_party/gpt-sovits-infer/` 下的 `pretrained_models/`、`G2PWModel/`、`runtime/`、`output/`、`GPT_weights_*/`、`SoVITS_weights_*/`、8 个 `dicts` 大文件），以及运行时改写的两个文件：上游每次建管线都会把权重绝对路径写回 `GPT_SoVITS/configs/tts_infer.yaml`，G2PW 词典重建后会改写 `GPT_SoVITS/text/g2pw/polyphonic.md5`。推理代码、参考音频、`manifest.json`、导出记录与音色注册表入库。`git add` 预演带的是自有文件（`.python-version`、`README.md`、`framework.md`、`pyproject.toml`、`uv.lock`、`.gitignore`、`main.py`、`fetch_tts.py`、`extract_infer.py`、`components.py`、`deploy.bat`、`deploy.sh`、`test/`）加 `third_party/gpt-sovits-infer/` 里的 169 个文件。四份权重不在仓库里，由 `fetch_tts.py voice` 从 ModelScope 拉。表里“入库”一列说的就是这个边界。

| 组件 | 来源 | 许可 | 入库 |
|---|---|---|---|
| GPT-SoVITS 推理代码 | `RVC-Boss/GPT-SoVITS` @ `b4570b8`，只抽合成链 | MIT，随上游 `LICENSE` 一起进 | 是，157 个文件 |
| 自家音色 `da_fei_yv` 与 `da_fei_yv_plus` 的权重与参考音频 | 本工程自训，训练数据 `YinZilu/da-fei-yu-girl-voice-zh`；权重发布在 ModelScope `Caoxiansong/da_fei_yv` | 训练数据是 **CC BY-NC 4.0**：仅限非商业，须署名「尹紫鹭幻想社（Bilibili）」，不得用于冒充真人。权重是衍生作品，同受此约束 | 权重否，`fetch_tts.py voice` 从 ModelScope 拉；参考音频 0.3 MB 入库 |
| 上游 git 里的词表与 CUDA 运行库 | 同一 commit 的 `raw.githubusercontent.com` | 随上游仓库（MIT） | 否，`fetch_tts.py dicts` 取，8 个 35.3 MB，逐个校验 SHA256 |
| chinese-hubert-base、chinese-roberta-wwm-ext-large、G2PWModel、nltk_data | 上游 `XXXXRT/GPT-SoVITS-Pretrained` 的 zip | 以各自来源页声明为准，本仓库不重新授权 | 否 |
| `lid.176.bin` | fastText 官方 supervised models | CC BY-SA 3.0（fastText 文档页写明，不是常见的 NC 误传） | 否 |
| FFmpeg 共享库 | BtbN `ffmpeg-master-latest-win64-gpl-shared` | GPL v3（该构建含 x264 等） | 否 |

内容改动核对过：抽取只做取舍，157 个文件与上游 commit 比，没有一处真实内容差异（按 LF 归一后逐字节相同）。剩下的差别全是行尾。上游 clone 用 `core.autocrlf=true` checkout，工作树是 CRLF；本仓库同样开 autocrlf，`git add` 时折回 LF。实测入库后的 blob 哈希有 128 个与上游一致，另外 29 个（`Accel/` 下的引擎文件、`text/korean.py`、`text/cantonese.py`、`process_ckpt.py`、`sv.py`、`tools/audio_sr.py` 等）上游自己就是把 CRLF 提交进 git 的，入库后只差行尾，代码字符一致。

对外发布要守三条：

1. 不要用 `git add -f` 把被忽略的路径塞进仓库。预训练模型和 GPL 二进制一旦进历史就很难干净撤回。
2. 权重是自己训的，但训练数据是 CC BY-NC 4.0，权重是衍生作品，同样受非商业与署名约束。仓库首页、发行物和对外演示都要写明音色数据来自「尹紫鹭幻想社（Bilibili）」，按 CC BY-NC 4.0 使用。
3. 四个权重单个在 129 MiB 与 165 MiB 之间，超过 GitHub 普通 git 对象的 100 MB 上限，LFS 又要占配额，所以放到 ModelScope（`Caoxiansong/da_fei_yv`），由 `fetch_tts.py voice` 匿名直链拉取并校验 SHA256。CC BY-NC 4.0 的衍生权重本来也不适合再分发到 GitHub。

ModelScope 上的许可字段要单独改一次。仓库建好时平台的 `License` 默认是 `Apache License 2.0`，权重实际的许可却是 CC BY-NC 4.0，两者冲突。README 里的 front matter 改不了这个字段，要在网页的模型信息里把 License 选成 CC BY-NC 4.0。字段不改，别人只看平台标签就会以为可以商用。

历史核对：`git log --all -- resource/file_server/third_party/gpt-sovits-infer` 只输出引入推理子集的那次提交，整仓上游 clone、第三方权重与二进制从未进过任何提交。权重只存在于 ModelScope，仓库里留下的是参考音频与导出记录。推理子集是首次入库的第三方代码，MIT 允许再分发，保留 `LICENSE` 和出处记录即可，公开这个仓库不需要改写历史。

