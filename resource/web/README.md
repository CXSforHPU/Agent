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
│       │   ├── device/        设备增删改查、激活凭据、探活、配置下发
│       │   ├── card/          能力卡增删改查与设备绑定
│       │   ├── conversation/  会话与消息
│       │   ├── a2a/           A2A 协议模型与设备侧 WebSocket 服务
│       │   ├── agent/         网站侧 agent
│       │   ├── chat/          聊天入口，按 deviceId 分流
│       │   ├── ops/           运维面板：装依赖、起停服务、试 TTS 与 STT
│       │   ├── config/        跨域、JWT 过滤器、参数解析
│       │   └── common/        统一响应与异常处理
│       └── resources/
│           ├── application.yml
│           └── schema.sql     建表语句
├── client/           Vue3 前端，naive-ui 组件库
│   ├── public/
│   │   └── agent-resource-baseline.json  资源评估页加载的基线
│   ├── src/
│   │   ├── App.vue       后台外壳：侧边菜单、页头
│   │   ├── theme.js      品牌色与组件主题改写
│   │   ├── style.css     全局基础样式与通用类
│   │   ├── api.js        后端接口封装
│   │   ├── resourceModel.js  资源评估的默认数据与计算模型
│   │   ├── resourceReport.js 报告解析、可信状态与校准
│   │   ├── firmwareReport.js 在浏览器里解析 ELF、map 与 rtconfig.h
│   │   └── views/        九个页面
│   └── vite.config.js
└── .run/             日志与运行产物，不进版本库
```

## 前端

Vue 3 加 Vite，组件库用 [naive-ui](https://www.naiveui.com/)，图标用 `@vicons/ionicons5`。naive-ui 按需引入，`<script setup>` 里直接 import 组件，不装额外的解析插件。

页面骨架是一个左侧菜单加右侧内容区。登录页不套这个外壳，单独渲染。菜单八项对应八个页面：

| 页面 | 路径 | 内容 |
|---|---|---|
| 总览仪表盘 | `/dashboard` | 设备、能力卡、会话与事件的统计 |
| 设备管理 | `/devices` | 设备增删改查、激活凭据、探活、配置下发。列表用表格，上面三块统计数字 |
| 能力卡 | `/cards` | 能力卡增删改查，绑定或解绑设备。列表带每张卡绑了几台设备 |
| 资源评估 | `/planner` | 按片内 Flash、RAM 与外部存储容量推荐模块组合，估算占用比例，可导出 Kconfig 与 JSON |
| 对话 | `/chat` | 一个面板一个会话，一屏可以并排开多个。左边选对话对象与会话记录，右侧是面板区，每个面板有独立的对话对象、会话、消息流与输入框。回复一段段推过来，思考过程与工具调用实时显示 |
| 会话历史 | `/history` | 左边会话列表，右边消息详情 |
| 设置 | `/settings` | 网站侧 agent 的五个配置项 |
| 运维 | `/ops` | 文件服务器与 MCP 网关的七个面板 |

主题集中在 `client/src/theme.js` 一个文件里。品牌色、圆角、字体、表格与卡片的样式都在这儿改，`App.vue` 用 `NConfigProvider` 挂上去。想换品牌色改 `brandColor` 一个值就行，不用翻各个页面。

侧边菜单与页头在 `App.vue` 里，中文用 naive-ui 自带的 `zhCN` 语言包。

交互上的两个约定：

- 用户没改过的输入框不会被状态轮询刷回去。运维页的端口和组件勾选都靠这个，改过的项在用户保存前保持原样。
- 组件勾选的「未保存」标记挂在 `NCheckboxGroup` 的 `update:value` 上，不挂在单个 `NCheckbox` 上。naive-ui 的子复选框在组里不发 `update:checked`，挂错地方「保存勾选」会一直是灰的。

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

这五项只影响网站侧 agent。端侧设备的 LLM 配置目前通过固件配置。

## 资源评估页

`/planner` 页面按设备容量估算 Agent 方案的资源占用，不连后端。默认数字写在 `client/src/resourceModel.js`，页面启动时再加载 `client/public/agent-resource-baseline.json`，覆盖固定体积、模块差值和线程栈默认值。基线加载失败就退回内置常量。

基线由 `scripts/resource_report.py baseline` 从评测矩阵和基线 map 生成：

```bash
python scripts/resource_report.py baseline \
  --matrix build/memeval/matrix.json \
  --map build/memeval/map_M0.map
```

默认输出到 `client/public/agent-resource-baseline.json`。换版本重新构建、跑完矩阵后重跑一次，刷新页面就能拿到新数字。

实测报告用同脚本的 `collect` 生成，读 ELF、map、`rtconfig.h` 和可选的运行时栈数据：

```bash
python scripts/resource_report.py collect \
  --elf rtthread.elf --map rtthread.map --config rtconfig.h \
  --runtime stacks.json -o agent-report.json
```

`--runtime` 是一个 JSON 文件，`stacks` 数组每项形如 `{"name": "AgentLoop", "bytes": 10240, "watermark": 0.31}`。报告只保留白名单里的开关和数值，不含地址、账号、密钥或模型名。

不想跑脚本时，直接点「选择固件目录」，选工程根目录或 build 目录里带 `rtthread.elf`、`rtconfig.h` 的那一层。`rtthread.map` 与 `rtthread.bin` 在同一层就一起带进来，也可以点「选择固件文件」逐个选。页面会解析 ELF 节表、map 归属和 `rtconfig.h` 白名单，生成与 `collect` 同格式的报告，存在浏览器里。文件只在浏览器里读，不上传控制台。

页面内解析拿不到运行时栈水位，要水位还是用 `collect --runtime` 生成报告再导入。

拿到报告后，卡片右上角给出可信状态。模块开关、Kconfig 栈和运行时栈都对得上时是「当前配置实测」；只对上一部分时是「同环境估算」，并列出不一致项；没报告是「参考估算」。「用报告校准现有固件」把整机实测减去当前方案估算，填进现有固件两栏。报告与页面配置分别存在 `agent-console-planner-report-v1` 和 `agent-console-planner-v1` 里。

输入分五组：

| 输入 | 作用 |
|---|---|
| 片内 Flash、片内 RAM | 算占用比例和余量，不填或填 0 会提示 |
| SPI Flash、SD 卡 | 估算 OTA 备份和记忆文件放不放得下 |
| 现有固件 Flash、静态 RAM | 已有工程时填 Agent 之外的占用，避免从零重算 |
| 已包含的依赖 | 勾上 mbedTLS、lwIP 或 newlib 后，对应体积从新增占用里扣除 |
| 线程栈 | 按 KB 调 AgentLoop 上下文栈、A2A、文件操作、播报与录音、MQTT 收发线程的栈，直接计入 RAM 占用 |

线程栈默认值来自基线，面板上同时显示实测水位。AgentLoop 上下文栈默认 10 KB，离线回归峰值 3.3～3.8 KB，长上下文或更大响应可以调高；调高后 RAM 占用和推荐档位立刻跟着变。文件操作与 MQTT 接收栈走 Kconfig，其余栈写死在源码的线程创建参数里，导出的 Kconfig 片段会带上当前值或改法注释。模块关闭时对应的栈输入置灰，不参与计算。导入报告后，峰值超过当前预算的行标「预算偏低」。

模块开关按实测差值叠加，推荐区会按当前容量从完整功能、设备控制、记忆增强、最小对话四档里挑第一档放得下的。安全余量默认 Flash 15%、RAM 20%，超了就标红并给出差额。

页面上的数字分两类：Flash 模块差值、静态 RAM、线程栈来自完整固件实测；技能只留 1 份、只编译一种协议、语音播报这三项是估算。数据口径和复现方法见 `docs/resources.md`。SPI Flash 与 SD 卡只放文件、记忆和 OTA 备份，不能直接扩容片内 Flash 或 RAM。

「复制 Kconfig」给出与当前开关对应的配置片段；「导出 JSON」把输入、明细和推荐结果一起存下来。配置存在浏览器的 `agent-console-planner-v1` 里，刷新不丢。

## 运维页

`/ops` 页面管板外的文件服务器（`resource/file_server` 下那套 FastAPI）和它带的 MCP 网关。页面分七个面板：

| 面板 | 做什么 |
|---|---|
| 组件状态 | 探文件服务器和 MCP 网关的端口，看 `.venv` 和 TTS 权重在不在。每 5 秒自动刷一次 |
| 安装依赖 | 后台任务。装前端依赖、文件服务器依赖分开装，语音合成与语音转写各一个按钮 |
| 组件依赖明细 | 四个组件各缺什么，逐项列出依赖与版本 |
| 文件服务器与 MCP 网关 | 勾选要启动的组件，启动、停止、改两个服务的端口 |
| 语音合成试听 | 选音色，合成一段 WAV 直接在页面里播，合成期间有进度条 |
| 语音转写 | 丢一个音频文件进来，转成文本 |
| 日志 | 读文件服务器日志或装依赖日志的尾部，新行自动滚到底 |

### 组件

整套服务拆成四个组件，装依赖与起服务都按组件点名：

| 组件 | 显示名 | 说明 |
|---|---|---|
| `file-server` | 文件服务器 | 文件上传下载，FastAPI 那一套。本体，不能关 |
| `mcp` | MCP 网关 | 板端连的 MCP 端点，端口默认 8765 |
| `tts` | 语音合成 | `/tts` 与 `/tts/play`，要装 GPT-SoVITS 那条链 |
| `stt` | 语音转写 | `/stt`，本地 whisper 识别 |

「组件依赖明细」面板是 `file_server/components.py status` 的结果。它只看文件和已装的包，不下载任何东西。每一项依赖带三种标记：必需依赖齐了是 `✓`，缺了是 `✗`，可选依赖是 `○`。探测要起子进程，第一次可能要几十秒，面板上有「重新探测」按钮。

「安装依赖」面板按组件分开装。点「安装语音合成依赖」走的是 `deploy.bat --install file-server,tts --check`，会拉 GPT-SoVITS 的推理代码、预训练模型和音色权重，第一次要下载几个 GB。

装依赖是后台任务。点完按钮返回一个任务 id，前端每 2 秒轮询 `/api/ops/jobs/{id}`，输出重定向到 `.run/install.log`。

Windows 上文件服务器在跑时装依赖会失败，`uv sync` 换不了 `torch/lib/c10.dll`，报「拒绝访问」（os error 5）。先点「停止」再装。

### 启动哪些组件

「文件服务器与 MCP 网关」面板里有一组勾选框，四个组件各一个，文件服务器那个是禁用的，它是本体，关不掉。勾选存到后端的 SQLite 里，键是 `ops_setting` 表的 `startComponents`，数据库文件在 `resource/web/server/data/` 下，跟着 `.gitignore` 走，个人配置不进版本库。

点「保存勾选」存下选择，点「按勾选启动」按这个选择起服务。启动走 `deploy.bat --start-only --start <组件列表>`，没勾的组件不注册端点，调用它返回 404。默认全选。

服务还在跑的时候改勾选不拦，保存后下次启动生效。

### 启停

启动走同一个脚本的 `--start-only`，连带把勾选的 MCP 网关拉起来。脚本在前台跑，后端等端口起来再回话，最多等 90 秒。

停止分两步。先杀本后端实例起的进程；后端重启过或者服务是手工脚本起的，本实例手上没有进程，就按端口反查 pid 再杀一次。Windows 上用 `netstat -ano` 查 pid，`taskkill /T /F` 按进程树杀，只结束 `cmd.exe` 的话 python 子进程还占着端口。MCP 网关按 `resource/file_server/mcp_gateway.pid` 里的 pid 停，没有 pid 文件就按端口反查。

### 改端口

「文件服务器与 MCP 网关」面板里有两个端口输入框：文件服务器端口和 MCP 网关端口。都填 1~65535，点「保存端口」，只发改过的那个。存的值放在后端的 SQLite 里，键是 `ops_setting` 表的 `fileServerPort` 和 `mcpPort`，数据库文件在 `resource/web/server/data/` 下，跟着 `.gitignore` 走，个人配置不进版本库。

规则有四条：

1. 服务还在跑的时候改它的端口会被拒绝，返回 409。文件服务器在跑就改不了文件服务器端口，MCP 网关在跑就改不了 MCP 网关端口，先点「停止」再改，不然旧端口上的进程会变成没人管的孤儿。
2. 保存后要下次启动才生效。启动脚本按这个端口起文件服务器，MCP 网关的 `--mcp-port` 和 `--file-server` 地址都跟着变。
3. 两个端口不能填一样，会回 400。
4. 没保存过就用 `agent.console.file-server-port` 和 `agent.console.mcp-port` 的默认值。

板端连网关的地址是 `http://PC-IP:<MCP 网关端口>/mcp`，也就是固件里的 `PKG_AGENT_MCP_SERVER_URL`。改了 MCP 端口，板端那台固件要重新配一遍。固件选了自动获取地址就不用重新配，只填控制台地址，板端启动时自己来问，见下面「板端地址」。

### 板端地址

「板端地址」填板子访问文件服务器和 MCP 网关用的主机名，只填主机名或 IP，不带协议和端口。QEMU 走 TAP 网桥接宿主机时填 `192.168.137.1`，真板子填它在局域网里的 IP。

存的值放在 `ops_setting` 表的 `deviceHost` 键里。默认值取 `agent.console.device-host`，没配就用 `agent.console.file-server-host`，再没有就用 `127.0.0.1`。

这个值和控制台自己用的 `fileServerHost` 是两回事。控制台在板上，能用 `127.0.0.1` 连本机的文件服务器；板子在控制台外面，只能用板子够得着的地址。所以控制台报给板端的地址是 `http://<deviceHost>:<端口>`。

固件在 `External Server` 菜单里选了自动模式，启动时请求 `GET /api/ops/endpoints`，拿回板端该用的两个地址。这种模式下固件只填控制台地址，另外两个由这个接口下发。这个接口免登录，板手上没有令牌。回包里是：

```json
{
  "success": true,
  "message": "ok",
  "data": {
    "deviceHost": "192.168.137.1",
    "fileServerUrl": "http://192.168.137.1:8000",
    "fileServerPort": 8000,
    "mcpUrl": "http://192.168.137.1:8765/mcp",
    "mcpPort": 8765
  }
}
```

改端口或者改板端地址，板端下次启动就能拿到新地址，不用重新烧固件。板端启动时控制台连不上，它用固件里编译进去的默认地址继续跑。

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

### 日志自动跟随

日志面板每 3 秒拉一次尾部（`GET /api/ops/log`），有新行就自动滚到底。往上翻过就停跟随，这时「回到最新」按钮会出现，滚回底部或者点它就跟上。停跟随期间新行照样进来，滚动位置会补回去，视线不会跳。

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
| GET | `/api/devices` | 设备列表，含激活状态与在线状态 |
| GET | `/api/devices/ws/status` | 需 JWT；返回监听配置与当前账号已登记 WebSocket 设备的连接状态，不含 token |
| POST | `/api/devices` | 新建设备。带 `username`、`password` 时一并登记激活凭据，响应里回一次激活配置 |
| POST | `/api/devices/{id}/credential` | 重设激活账号密码。已经激活的设备改不了，返回 409 |
| PUT | `/api/devices/{id}` | 修改设备 |
| DELETE | `/api/devices/{id}` | 删除设备 |
| POST | `/api/devices/{id}/probe` | 探活一次，同时返回能力卡 |
| POST | `/api/devices/{id}/config` | 配置下发 |
| GET | `/api/devices/{id}/card` | 读这台设备绑定的能力卡 |
| PUT | `/api/devices/{id}/card` | 绑定或解绑能力卡，body 是 `{"cardId": 1}`，解绑传 `null` |
| POST | `/api/devices/{id}/card` | 能力卡下发，板端存 SD 卡 |
| GET | `/api/devices/{id}/events` | 这台设备最近的上报事件，新的在前 |
| GET | `/api/devices/{id}/events/stream` | 上报事件流，板端一上报网页立刻收到 |
| GET | `/api/cards` | 能力卡列表，带各自的设备数 |
| POST | `/api/cards` | 新建能力卡 |
| PUT | `/api/cards/{id}` | 修改能力卡 |
| DELETE | `/api/cards/{id}` | 删除能力卡 |
| POST | `/api/cards/{id}/bind` | 绑定设备，body 是 `{"deviceId": 2}` |
| POST | `/api/cards/{id}/unbind` | 解绑设备，body 是 `{"deviceId": 2}` |
| GET | `/api/conversations` | 会话列表，可按 `deviceId` 过滤 |
| GET | `/api/conversations/{id}` | 会话详情，含全部消息 |
| POST | `/api/conversations` | 新建会话 |
| DELETE | `/api/conversations/{id}` | 删除会话 |
| POST | `/api/chat` | 发消息，`deviceId` 为空走网站侧 agent |
| POST | `/api/chat/stream` | 同上，但用 SSE 一路推中间过程 |
| GET | `/api/settings` | 读当前用户的设置项、当前值与 `rtconfig.h` 默认值 |
| PUT | `/api/settings` | 保存设置，body 是 `{key: value}` |
| POST | `/api/settings/{key}/reset` | 把某一项恢复成环境变量默认值 |
| GET | `/api/ops/status` | 各组件状态，含当前启动组件清单 |
| GET | `/api/ops/settings` | 读运维设置，含两个服务的当前端口、板端地址、默认值与启动组件清单 |
| PUT | `/api/ops/settings` | 改运维设置，body 是 `{"fileServerPort": N, "mcpPort": M, "deviceHost": "192.168.137.1", "startComponents": ["mcp"]}`，四项都可选 |
| GET | `/api/ops/endpoints` | 板端用的文件服务器与 MCP 地址。免登录，固件启动时来问 |
| GET | `/api/ops/components?refresh=` | 各组件的依赖明细，`refresh=true` 时跳过缓存重新探测 |
| POST | `/api/ops/install?target=&components=` | 后台装依赖，返回任务 id。`target` 取 `web` 或 `file-server`，`components` 是要装的组件，逗号分隔 |
| GET | `/api/ops/jobs` | 装依赖的任务列表 |
| GET | `/api/ops/jobs/{id}` | 单个任务的进度 |
| POST | `/api/ops/start` | 按勾选的组件起文件服务器与 MCP 网关 |
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

`POST /api/chat/stream` 的请求体一样，返回 `text/event-stream`，每行一个 JSON：

```json
{"kind": "reasoning", "text": "用户想问…"}
{"kind": "final", "text": "完整答案", "conversationId": 1}
```

`kind` 取 `status`（开始处理）、`reasoning`（思考分片）、`tool`（工具调用）、`content`（正文分片）、`final`（完整答案，同时带上会话 id）、`error`（出错）。前面几类是增量，逐条追加显示；`final` 是权威答案，用来覆盖增量拼出来的文本。`conversationId` 为空表示这次还没落库。

## 设备主动连接的 WebSocket 通道

板端选择 `PKG_AGENT_A2A_CHANNEL`，主动连接 Web 后端的 `/api/a2a/ws`。
`PKG_AGENT_A2A_HOST`、`PKG_AGENT_A2A_PORT`、`PKG_AGENT_A2A_WS_ENDPOINT` 指定真实服务器。
设备表里只填 `ws://设备ID`，这是逻辑标识，不能当作网络 URL 用。

### 激活

设备有四个状态：未激活、激活中、在线、未在线。

未激活的板子不自动连。等板端执行 MSH 命令：

```
a2a_activate [host [port [username [password]]]]
```

账号密码的取值顺序是命令参数、凭据文件 `/agent_a2a_cred`、Kconfig。
运行时文件排在 Kconfig 前面，同一份固件换张 SD 卡就能换凭据。
密码不回显，也不写进日志。后端只存 PBKDF2 加盐摘要，不存明文。

服务端校验后签发一次性设备 token。板端把它写进 `/agent_a2a_token`，
之后凭 token 重连，密码不再出现。

一台设备只能激活一次。后端对这些情况回错误帧后断开：

| 错误码 | 含义 |
|---|---|
| `bad_credential` | 账号或密码不对 |
| `already_online` | 同一个设备 ID 已经在线 |
| `already_activated` | 已经激活过，不换密码也不换 token |
| `address_taken` | 同一个逻辑地址被另一台已激活设备占着，删掉旧设备行再激活 |
| `device_not_found` | 控制台里没这台设备，或它没登记这份凭据 |
| `bad_token` | token 不认识 |

控制台删掉设备行以后才能重新登记并激活。改密码只在未激活时允许。

### 帧格式

连接建立后板端发 `hello`，服务端回 `welcome`，再把绑定的能力卡推过来。
之后板端收 `ping`、`card`、`task`，回 `pingResult`、`cardResult`、`taskResult`。
每个请求带 `id`，应答回同一个 `id`。

JSON 文本消息，单条最多 8192 字节。服务端每 20 秒发一帧 ping，
`timeout + 30` 秒没收任何帧就断开，设备状态随即变成未在线。

任务只返回 final，能力卡 `streaming=false`。自发结论走同一连接的 `report` 事件，
带 `kind` 与 `text`，后端按板子地址归到设备行，再推给正在听 SSE 的网页。

同一设备请求串行执行。超时或断线不自动重放任务；旧任务超时后，必须先消费
并丢弃迟到结果，才接受新任务。板端每 3 秒重连一次。主动上报用 4 条有界队列，
满了丢最旧事件，发送失败不重放。

当前板端只支持可信网络内的明文 `ws`，没有实现带证书校验的 `wss`。
不要把它当作公网安全传输。

### 能力卡

能力卡放在独立的 `/api/cards` 下。一个能力卡可以绑多台设备，一台设备只能绑一个能力卡。
给已经绑了别的卡的设备再绑一次会返回 409。

板端在线时，改完绑定关系立刻把新卡推过去。板端离线时，绑定关系已经落库，
下次连上时服务端会补推。板端收到卡后回 `cardResult`，里面是当前生效的能力卡。

### 多会话同屏

对话页是一个面板一个会话，一屏可以并排开多个。左边栏选对话对象，「打开」按钮按当前选中的对象开一个面板；点会话记录里的一条，把那个会话开到面板里，已经在面板里打开过的会话会标「已打开」，再点一次是重新加载历史。面板右上角的「新会话」清掉面板与会话的绑定，「×」关掉面板。

面板之间互不干扰：各自持有对话对象、会话、消息与输入框，一个面板在等回复时另一个照常能发。端侧 agent 的上报流按设备复用，同一台设备只开一条 SSE，多个面板各自订阅。

实现上 `panels` 是一个响应式数组，面板对象用 `reactive()` 建：上报流回调是异步进来的，如果面板是普通对象，回调里改字段不会触发重绘，页面上看不出新消息。

### 网站侧 agent 与端侧 agent 通信
不选设备时对话走网站侧的 agent，它带一个 `ask_device` 工具：模型按设备名找到地址，经 A2A 把问题转给端侧 agent，拿到回答再作答，最多来回 4 轮。这个账号的设备列表会进系统提示词，模型才知道能对哪台设备提问。

端侧 agent 跑在设备上，能读传感器、下发指令、连 MQTT 和 MCP；网站侧 agent 只是控制台里的助手，问不到设备内部的东西，需要转一手。工具实现是 `server/src/main/java/com/agentconsole/agent/DeviceTool.java`。

## 验证

运行 `python scripts/mqtt_test/websocket_e2e_test.py`。脚本使用真实 Spring 后端、
独立 QEMU 和 SD 副本。四组判据：

1. 密码错的时候握手就被拒，回 `bad_credential`，板子没有激活。
2. 密码对时 `a2a_activate` 打印 `[OK]`，token 落到 SD 卡，设备状态变成在线。
3. 能力卡绑定正确，探活返回绑定的卡；再发一个计算任务，收到非空的 final。
4. 同一台设备再连一次回 `already_online`；再拿密码激活一次回 `already_activated`。

结果在 `server/target/ws-test/`。LLM 失败提示只证明最终结果回传，不能算任务成功。
浏览器在 `http://127.0.0.1:5173` 打开设备页的“WebSocket 连接”，再到聊天页测试。

### 运维页的验证

运维页要单独走一遍，十二步：

1. 组件状态里文件服务器显示未就绪，TTS 权重显示未就绪，说明依赖还没装。
2. 点「重新探测」，组件依赖明细列出四个组件的依赖。缺的打 `✗`，装好的打 `✓`，可选的用 `○`。
3. 点「安装文件服务器依赖」，任务状态从 `running` 变成 `done`，退出码 0。进度看 `.run/install.log`。
4. 点「安装语音转写依赖」，任务状态也变成 `done`，退出码 0。装完再点一次「重新探测」，语音转写那一行的 `✗` 变成 `✓`。
5. 在「启动哪些组件」里取消 MCP 网关和语音合成，点「保存勾选」，提示启动组件已保存。
6. 点「按勾选启动」，提示文件服务器已在 `http://127.0.0.1:8000`。`/stt` 有响应，MCP 网关端口没监听。
7. 点「停止」，两个端口回到未就绪。
8. 四个组件都勾上，点「保存勾选」再点「按勾选启动」。组件状态里 MCP 网关变成就绪，地址是 `http://127.0.0.1:8765/mcp`。
9. 点「读取音色列表」，下拉框里出现文件服务器报的音色名。没装语音合成依赖时这一步报 503。
10. 点「合成并播放」，进度条先走一段来回动的条，旁边显示已用时长；合成结束后条跳到 100%，页面出现播放器并能出声。再丢一个音频文件到语音转写，回显识别出的文本。
11. 在「文件服务器端口」或「MCP 网关端口」里填一个新端口点「保存端口」，提示服务还在跑、改不了。点「停止」之后再改，这次保存成功，提示下次启动生效。两个端口填一样会被拒绝。再点「按勾选启动」，两个服务的地址都换成新端口。
12. 在日志面板等着新行进来，面板自己滚到底。往上翻过就停跟随，「回到最新」按钮出现，点它又跳回底部。最后点「停止」，提示停掉了进程。

合成的第一次要冷启动 GPT-SoVITS，本机实测整机要跑十分钟上下，等这么久都正常。

## 已知限制

- A2A 通道只支持明文 `ws`，没有 `wss`。只放在可信网络里用。
- 一台设备只能激活一次。要换账号或换板子，先删掉设备行再重新登记。
- 同一个板子地址只能有一条激活记录。两个账号不能各激活一次同一个地址。
- 网站侧 agent 的 API Key 在设置页、环境变量和 `rtconfig.h` 里都拿不到时，`/api/chat` 会返回「还没配置 API Key」。
- 会话历史存在 SQLite 里，没有清理策略，长期运行需要自己定。
- 前端开发服务器在 5173，代理到 8080 的规则写在 `client/vite.config.js`，换端口要一起改。
- `client/.npmrc` 把 npm 源指到华为云镜像，这台机器直连 `registry.npmjs.org` 超时。能直连的环境删掉这个文件即可。
- 测试时 curl 发中文会被按 GBK 编码，服务端按 UTF-8 解析会报 `Invalid UTF-8 start byte`。命令行测接口用英文，或用文件传 UTF-8 请求体。
- 运维页的路径相对 `resource/web/server/`。从别的目录直接 `java -jar` 起后端，文件服务器目录、日志目录都会指错。
- 停止按钮在本实例没有进程时按端口反查 pid。占着文件服务器或 MCP 网关端口的别的进程会被一起杀掉。
- 换过 torch 的版本号之后，`site-packages` 里可能留下旧版本的 `torch-*.dist-info` 空壳目录。`importlib.metadata` 扫到它就取不到版本号，`transformers` 导入时会报 `InvalidVersion: Invalid version: None`，合成直接 500。查一下 `resource/file_server/.venv/Lib/site-packages/` 里有没有多余的 `torch-*.dist-info`，把没内容的那个挪走，再重启文件服务器。
