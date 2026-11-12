# Agent 管理控制台

服务器上的管理网站。做三件事：账号注册登录、设备管理、和 agent 对话。对话有两条路，一条走网站侧的 agent，一条经 A2A 转给端侧设备上的 agent。

## 目录结构

```
resource/web/
├── setup.sh          一键安装与启动
├── server/           Spring Boot 后端
│   ├── pom.xml
│   ├── .jdk/         免安装 JDK，不进版本库
│   ├── .maven/       免安装 Maven，不进版本库
│   ├── data/         SQLite 数据库文件，不进版本库
│   └── src/main/
│       ├── java/com/agentconsole/
│       │   ├── auth/          注册、登录、JWT
│       │   ├── device/        设备增删改查、探活、配置下发
│       │   ├── conversation/  会话与消息
│       │   ├── a2a/           A2A 协议模型与客户端
│       │   ├── agent/         网站侧 agent
│       │   ├── chat/          聊天入口，按 deviceId 分流
│       │   ├── ops/           运维面板：装依赖、起停服务、试 TTS 与 STT
│       │   ├── config/        跨域、JWT 过滤器、参数解析
│       │   └── common/        统一响应与异常处理
│       └── resources/
│           ├── application.yml
│           └── schema.sql     建表语句
├── client/           Vue3 前端
└── .run/             日志与运行产物，不进版本库
```

## 启动

```bash
cd resource/web
./setup.sh              # 装依赖、编译、启动前后端
./setup.sh --install    # 只装依赖
./setup.sh --server     # 只启动后端
./setup.sh --client     # 只启动前端
./setup.sh --stop       # 停掉前后端
```

Windows 下双击或在 cmd 里跑 `setup.bat`，参数一样：`--install`、`--server`、`--client`、`--stop`。两个脚本只管 web 前后端，不碰 MCP 网关和固件编译。

脚本用包内的 JDK 与 Maven，不改系统 PATH。系统里已经装了 `java` 时优先用系统的。JDK 约 190 MB，第一次运行会从清华镜像下载，之后复用 `server/.jdk/`。

起来之后：后端在 `http://127.0.0.1:8080`，前端在 `http://127.0.0.1:5173`。

## 环境变量

| 变量 | 作用 | 默认值 |
|---|---|---|
| `AGENT_JWT_SECRET` | JWT 签名密钥，至少 32 字符 | 内置占位值 |
| `AGENT_RTCONFIG_H` | `rtconfig.h` 的路径 | 空，从工作目录往上找带 Agent 包的工程根 |
| `AGENT_LLM_PROTOCOL` | 网站侧 agent 的协议，`openai` 或 `anthropic` | 空，用 `rtconfig.h` 里选中的那个 |
| `AGENT_LLM_API_KEY` | 网站侧 agent 的模型密钥 | 空，用 `rtconfig.h` 的 `PKG_AGENT_API_KEY` |
| `AGENT_LLM_BASE_URL` | 模型接口的服务根地址，不含协议路径 | 空，用 `rtconfig.h` 里当前协议对应的 `PKG_AGENT_API_BASE_URL_*` |
| `AGENT_LLM_MODEL` | 模型名 | 空，用 `rtconfig.h` 的 `PKG_AGENT_MODEL_NAME` |
| `AGENT_CONSOLE_DB` | SQLite 文件路径 | `./data/agent-console.db` |

**生产环境必须覆盖 `AGENT_JWT_SECRET`**，用内置占位值签的令牌谁都能伪造。

网站侧 agent 的协议、地址、API Key、模型名默认取工程的 `rtconfig.h`，也就是编译进端侧固件的那份配置。取值优先级：设置页按账号存的值 → 环境变量 → `rtconfig.h` → 空。服务端启动时从工作目录往上找带 `PKG_USING_AGENT` 的 `rtconfig.h`，找到会在日志里打一行 `读到 rtconfig.h：...`；找不到就只用环境变量和内置默认值。

## 设置页

`/settings` 页面改的是网站侧 agent 的配置，按账号存，五个设置项：

| 设置项 | 作用 | 留空时 |
|---|---|---|
| `llm.protocol` | 采用的协议，OpenAI 或 Anthropic | 回退环境变量，再回退 `rtconfig.h` 里选中的那个 |
| `llm.baseUrl` | 协议地址，服务根地址不含协议路径 | 回退 `AGENT_LLM_BASE_URL`，再回退 `rtconfig.h` 里当前协议对应的 base URL |
| `llm.apiKey` | 服务商密钥 | 回退 `AGENT_LLM_API_KEY`，再回退 `rtconfig.h` 的 `PKG_AGENT_API_KEY` |
| `llm.model` | 模型名 | 回退 `AGENT_LLM_MODEL`，再回退 `rtconfig.h` 的 `PKG_AGENT_MODEL_NAME` |
| `agent.systemPrompt` | 系统提示词 | 用内置默认提示词 |

页面会把 `rtconfig.h` 给出的默认地址和默认模型名一并显示出来，密钥只露尾四位。切了协议，默认地址跟着换用 `rtconfig.h` 里对应协议的那份。

值存在 SQLite 的 `user_setting` 表里，数据库文件在 `.gitignore` 的 `server/data/` 下，**个人配置不会进版本库**。`AGENT_JWT_SECRET` 和 `AGENT_CONSOLE_DB` 不在设置页里，改它们要动环境变量并重启。

这五项只影响网站侧 agent。端侧设备的 LLM 配置走设备自己的 `/config` 接口，在设备管理页下发。

## 运维页

`/ops` 页面管板外的两个服务：文件服务器（`resource/file_server` 下那套 FastAPI）和它带的 MCP 网关。页面分六个面板：

| 面板 | 做什么 |
|---|---|
| 组件状态 | 探文件服务器和 MCP 网关的端口，看 `.venv` 和 TTS 权重在不在。每 5 秒自动刷一次 |
| 安装依赖 | 后台任务装前端依赖、文件服务器依赖、可选的 TTS 依赖 |
| 文件服务器与 MCP 网关 | 启动、停止、改文件服务器的端口 |
| 语音合成试听 | 选音色，合成一段 WAV 直接在页面里播，合成期间有进度条 |
| 语音转写 | 丢一个音频文件进来，转成文本 |
| 日志 | 读文件服务器日志或装依赖日志的尾部 |

装依赖是后台任务。点完按钮返回一个任务 id，前端每 2 秒轮询 `/api/ops/jobs/{id}`，输出重定向到 `.run/install.log`。文件服务器那条走 `deploy.sh` 或 `deploy.bat` 的 `--check`，带 TTS 的那条多一个 `--tts`，要下 GPT-SoVITS 的推理代码和音色权重，第一次几个 GB。

启动走同一个脚本的 `--start-only`，连带把 MCP 网关拉起来。脚本在前台跑，后端等端口起来再回话，最多等 90 秒。

停止分两步。先杀本后端实例起的进程；后端重启过或者服务是手工脚本起的，本实例手上没有进程，就按端口反查 pid 再杀一次。Windows 上用 `netstat -ano` 查 pid，`taskkill /T /F` 按进程树杀，只结束 `cmd.exe` 的话 python 子进程还占着端口。MCP 网关按 `resource/file_server/mcp_gateway.pid` 里的 pid 停，没有 pid 文件就按端口反查。

### 改文件服务器的端口

「文件服务器与 MCP 网关」面板里有个端口输入框，填 1~65535，点「保存端口」。存的值放在后端的 SQLite 里，键是 `ops_setting` 表的 `fileServerPort`，数据库文件在 `resource/web/server/data/` 下，跟着 `.gitignore` 走，个人配置不进版本库。

规则有三条：

1. 文件服务器还在跑的时候改端口会被拒绝，返回 409。先点「停止」再改，不然旧端口上的进程会变成没人管的孤儿。
2. 保存后要下次启动才生效。启动脚本按这个端口起文件服务器，MCP 网关的 `--file-server` 地址也跟着指向新端口。
3. 没保存过就用 `agent.console.file-server-port` 的默认值。

### 合成进度

合成走文件服务器的 `POST /tts`，返回的 WAV 塞进页面的 `audio` 标签。转写走 `POST /stt`，multipart 的 `file` 字段。

第一次合成要冷启动 GPT-SoVITS：装权重、编译 CUDA graph，本机实测整机要跑十分钟上下。合成期间页面每 700 毫秒轮询一次 `GET /api/ops/tts/progress`，拿到的进度有这些字段：

| 字段 | 说明 |
|---|---|
| `stage` | `idle`、`loading`、`running`、`done`、`error` |
| `index` | 已完成几句 |
| `total` | 一共几句，按引擎的切句结果算 |
| `audioSeconds` | 已生成的音频秒数 |
| `elapsed` | 这一轮已经跑了多少秒 |
| `message` | 一句话说明，带已用时长 |

GPT-SoVITS 的 `TTS.run()` 是一次性交回整段音频，句子级的进度要等它返回才有。所以模型加载和推理这两段里，句子数是空的，页面画的是一条来回走的进度条，配上服务端报的已用时长，让人看得出程序还活着。合成结束后进度条才会跳到 100%。

文件服务器没重启过就还没有这个接口，后端会回 502，页面上只是看不到进度条，合成本身照常。

日志读的是 `.run/file-server.log` 和 `.run/install.log`。这两个文件里混着编码：文件服务器写 UTF-8，GPT-SoVITS 和 torch 那些库往 stdout 写的是 UTF-16LE。服务端按 UTF-8 软解，坏字节替换掉，能读出大部分内容，不保证每个字符都对。

### 运维面板的配置

后端用 `agent.console.*` 下面这几个属性，路径都相对后端的运行目录：

| 属性 | 默认值 | 说明 |
|---|---|---|
| `agent.console.file-server-dir` | `../../file_server` | 文件服务器目录 |
| `agent.console.client-dir` | `../client` | 前端目录，装前端依赖时用 |
| `agent.console.run-dir` | `../.run` | 日志与运行产物 |
| `agent.console.file-server-host` | `127.0.0.1` | 探活和调服务的地址 |
| `agent.console.file-server-port` | `8000` | 文件服务器端口，运维页改过就以后者为准 |
| `agent.console.mcp-port` | `8765` | MCP 网关端口 |
| `agent.console.tts-timeout-seconds` | `900` | 合成请求的超时秒数，冷启动要十分钟上下，别调小 |

这些路径相对 `resource/web/server/`。要用 `setup.sh` 起后端；从别的目录直接 `java -jar`，路径会指错，运维页会显示文件服务器未就绪。

## 接口

除注册、登录、探活外都要带 `Authorization: Bearer <token>`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 探活 |
| POST | `/api/auth/register` | 注册，返回 token |
| POST | `/api/auth/login` | 登录，返回 token |
| GET | `/api/auth/me` | 当前用户 |
| GET | `/api/devices` | 设备列表 |
| POST | `/api/devices` | 新建设备 |
| PUT | `/api/devices/{id}` | 修改设备 |
| DELETE | `/api/devices/{id}` | 删除设备 |
| POST | `/api/devices/{id}/probe` | 探活一次，同时返回能力卡 |
| POST | `/api/devices/{id}/config` | 配置下发 |
| GET | `/api/conversations` | 会话列表，可按 `deviceId` 过滤 |
| GET | `/api/conversations/{id}` | 会话详情，含全部消息 |
| POST | `/api/conversations` | 新建会话 |
| DELETE | `/api/conversations/{id}` | 删除会话 |
| POST | `/api/chat` | 发消息，`deviceId` 为空走网站侧 agent |
| GET | `/api/settings` | 读当前用户的设置项、当前值与 `rtconfig.h` 默认值 |
| PUT | `/api/settings` | 保存设置，body 是 `{key: value}` |
| POST | `/api/settings/{key}/reset` | 把某一项恢复成环境变量默认值 |
| GET | `/api/ops/status` | 各组件状态 |
| GET | `/api/ops/settings` | 读运维设置，含当前文件服务器端口 |
| PUT | `/api/ops/settings` | 改运维设置，body 是 `{"port": N}` |
| POST | `/api/ops/install?target=&withTts=` | 后台装依赖，返回任务 id |
| GET | `/api/ops/jobs` | 装依赖的任务列表 |
| GET | `/api/ops/jobs/{id}` | 单个任务的进度 |
| POST | `/api/ops/start` | 起文件服务器与 MCP 网关 |
| POST | `/api/ops/stop` | 停文件服务器与 MCP 网关 |
| GET | `/api/ops/tts/speakers` | 可用音色 |
| POST | `/api/ops/tts` | 合成语音，返回 WAV 字节 |
| GET | `/api/ops/tts/progress` | 最近一次合成的进度 |
| POST | `/api/ops/stt` | 语音转写，multipart 的 `file` 字段 |
| GET | `/api/ops/log?name=&lines=` | 日志尾部，`name` 取 `file-server` 或 `install` |

响应统一是 `{success, message, data}`。出错时 `success` 为 `false`，`message` 写原因。

`PUT /api/devices/{id}` 是全量更新，`name` 与 `address` 必填，`groupName`、`note` 漏传按空串处理。

`POST /api/chat` 的请求体：

```json
{"message": "你好", "conversationId": 1, "deviceId": 2}
```

`conversationId` 不传就新建会话，`deviceId` 不传就走网站侧 agent。

## 端侧对接

网站经 A2A 协议和端侧 agent 通信，端点按协议约定：

| 路径 | 方法 | 用途 |
|---|---|---|
| `/.well-known/agent.json` | GET | 能力卡，探活也用它 |
| `/tasks/send` | POST | 同步发任务 |
| `/tasks/sendSubscribe` | POST | 流式发任务，SSE 返回 |

消息体是 `{"message": {"role": "user", "parts": [{"type": "text", "text": "..."}]}}`。同步接口返回 `{"id", "status": {"state"}, "artifacts": [{"parts": [...]}]}`，`state` 取 `working`、`completed`、`failed`。SSE 每一帧也是这个结构，空行分隔。

探活就是 GET 能力卡，200 算在线。设备的 `address` 字段填服务根地址，能力卡自动拼在后面。

端侧的 A2A 通道在 agent 包里，menuconfig 里选 `Agent interaction communication channel → A2A channel (Agent-to-Agent over HTTP)`。它跑在 WebNet 之上，端口用 `WEBNET_PORT`（默认 80），另外把 CGI 根切到 `/`，这样 `/.well-known/agent.json` 这种带点号的路径才能被匹配到。配套选项 `PKG_AGENT_A2A_NAME` 和 `PKG_AGENT_A2A_DESCRIPTION` 填能力卡里的名字和描述。

通道是互斥的，选了 A2A 就不能同时用 CLI、WebNet、Debug、LVGL。webnet 要求 lwIP 的 tcpip 线程栈大于 1408，工程里已把 `RT_LWIP_TCPTHREAD_STACKSIZE` 提到 2048。

配置下发走的是 WebNet 通道的 `POST /config`，端点是 `<address>/config`。只开了 A2A 通道的设备没有这个端点，接口会返回 502，不会假装成功。

同一台设备的请求在服务端是串行的：`A2aClient` 给每台设备加了一把锁，探活与聊天不会同时开两条连接。端侧 webnet 是单线程的，A2A 同步接口会占住整个线程直到 agent 跑完，并发时后到的连接会把先到那条的响应顶掉，客户端只收到空响应。

### 网站侧 agent 与端侧 agent 通信

不选设备时对话走网站侧的 agent，它带一个 `ask_device` 工具：模型按设备名找到地址，经 A2A 把问题转给端侧 agent，拿到回答再作答，最多来回 4 轮。这个账号的设备列表会进系统提示词，模型才知道能对哪台设备提问。

端侧 agent 跑在设备上，能读传感器、下发指令、连 MQTT 和 MCP；网站侧 agent 只是控制台里的助手，问不到设备内部的东西，需要转一手。工具实现是 `server/src/main/java/com/agentconsole/agent/DeviceTool.java`。

## 验证

控制台与端侧 agent 的链路要四步走通，缺一步都会表现成「对话没反应」：

1. **板子在跑**：QEMU 起来后串口里能看到 `A2A channel up on port 80`。板子用的是 QEMU 的 user 网络，宿主机一般只能通过 `hostfwd` 连进去，比如 `-net user,...,hostfwd=tcp::8081-:80`，设备地址就填 `http://127.0.0.1:8081`。
2. **服务端起来了**：日志里有 `读到 rtconfig.h：...`，说明默认配置来源对了；没有这一行就检查工作目录，或者用 `AGENT_RTCONFIG_H` 直接指定路径。
3. **探活通过**：设备管理页点探活，拿到能力卡算在线。拿不到就是地址或端口不对。
4. **三条对话链路**：
   - 选设备对话，走 A2A 转给端侧 agent；
   - 不选设备对话，走网站侧 agent，配置取自 `rtconfig.h`；
   - 不选设备、但让网站侧 agent 用 `ask_device` 去问设备，验证网页端与端侧的中继。

第 3、4 步的响应慢是正常的：端侧 agent 要真的调一次模型，一条消息几秒到十几秒。

### 运维页的验证

运维页要单独走一遍，八步：

1. 组件状态里文件服务器显示未就绪，TTS 权重显示未就绪，说明依赖还没装。
2. 点「安装文件服务器依赖（含 TTS）」，任务状态从 `running` 变成 `done`，退出码 0。进度看 `.run/install.log`。
3. 点「启动文件服务器与 MCP 网关」，提示文件服务器已在 `http://127.0.0.1:8000`，组件状态里两个端口都变成就绪。
4. 点「读取音色列表」，下拉框里出现文件服务器报的音色名。
5. 点「合成并播放」，进度条先走一段来回动的条，旁边显示已用时长；合成结束后条跳到 100%，页面出现播放器并能出声。再丢一个音频文件到语音转写，回显识别出的文本。
6. 在「文件服务器端口」里填一个新端口点「保存端口」，提示文件服务器还在跑、改不了。点「停止」之后再改，这次保存成功，提示下次启动生效。
7. 点「启动文件服务器与 MCP 网关」，组件状态里文件服务器的地址换成新端口，MCP 网关也跟着指过去。
8. 点「停止」，提示停掉了进程，两个端口回到未就绪。

合成的第一次要冷启动 GPT-SoVITS，本机实测整机要跑十分钟上下，等这么久都正常。

## 已知限制

- 网站侧 agent 的 API Key 在设置页、环境变量和 `rtconfig.h` 里都拿不到时，`/api/chat` 会返回「还没配置 API Key」。
- 会话历史存在 SQLite 里，没有清理策略，长期运行需要自己定。
- 前端开发服务器在 5173，代理到 8080 的规则写在 `client/vite.config.js`，换端口要一起改。
- `client/.npmrc` 把 npm 源指到华为云镜像，这台机器直连 `registry.npmjs.org` 超时。能直连的环境删掉这个文件即可。
- 测试时 curl 发中文会被按 GBK 编码，服务端按 UTF-8 解析会报 `Invalid UTF-8 start byte`。命令行测接口用英文，或用文件传 UTF-8 请求体。
- 运维页的路径相对 `resource/web/server/`。从别的目录直接 `java -jar` 起后端，文件服务器目录、日志目录都会指错。
- 停止按钮在本实例没有进程时按端口反查 pid。占着文件服务器或 MCP 网关端口的别的进程会被一起杀掉。
