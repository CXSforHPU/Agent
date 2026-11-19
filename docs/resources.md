# 存储与内存占用

这份文档给的是选型用的数字：占多少 Flash、多少静态 RAM、运行时用多少堆和栈。全部来自当前配置的完整构建和 QEMU 实测。改了 Kconfig 选项，数字要重新测。

## 1. 统计口径

Flash 数字取自 BSP 工作区根目录的 `rtthread.map` 和 `arm-none-eabi-size` 输出。构建用 `-Os`，链接开 `--gc-sections`。

包体只统计属于 Agent 包的 `.o` 文件，取 `.text`、`.rodata`、`.ARM.exidx`、`.ARM.extab`、`.data` 之和。链接时被整段丢掉的代码不计入，例如当前配置里的音频驱动。

运行时数据取自 QEMU 离线回归 `build/skill_qemu/`。每个 MSH 命令跑两次，检查堆增量与 utest 断言。

## 2. 当前配置

下面的数字对应的配置：

- MQTT 工具、设备框架工具、MCP 客户端都打开。
- 多模态、语音转写、音频驱动打开。
- A2A 走 WebSocket 通道。
- 技能用随包发布的 5 份。
- 分层记忆打开，工具一起打开；自动入库与持久化都关闭。
- OpenAI 与 Anthropic 两份协议都编译，运行时切换。

后面的开关对比都以这份配置为基线。基线变体是 `M0_mem_on`，记忆开、落盘关；其余变体只改一个开关，重新完整构建再测。

几个影响内存的默认值：

| 宏 | 默认值 | 作用 |
|---|---|---|
| `PKG_AGENT_MAX_REASONING_LEN` | 2048 | 思考过程缓冲 |
| `PKG_AGENT_MAX_CONTENT_LEN` | 2048 | 回答正文缓冲 |
| `PKG_AGENT_STREAM_LINE_BUFSZ` | 1024 | SSE 行缓冲，在栈上 |
| `PKG_AGENT_MAX_TOOL_ARG_LEN` | 512 | 单个工具参数上限 |
| `PKG_AGENT_MEMORY_CORE_ITEMS` | 8 | 核心记忆槽位，每轮全量注入 |
| `PKG_AGENT_MEMORY_RECALL_ITEMS` | 32 | 回忆记忆槽位，按关键词取前几条 |
| `PKG_AGENT_MEMORY_ITEM_BYTES` | 192 | 单条记忆的文本上限 |
| `PKG_AGENT_MEMORY_INJECT_BYTES` | 1024 | 每轮注入块的上限 |

## 3. Flash 占用

固件整体：text 2,843,475 字节，data 35,248 字节，bss 293,328 字节。

Agent 包本身是 229,595 字节，其中只读段 228,375 字节、data 1,220 字节，占 firmware text 加 data 的 8.0%。

### 关掉某个模块后固件小多少

从基线出发，只改一个开关，重新完整构建。下表是 `arm-none-eabi-size` 的实测差值，负值表示关掉后变小。

| 变体 | 改动 | text | data | bss |
|---|---|---|---|---|
| 记忆关 | `PKG_AGENT_MEMORY_ENABLE` 与 `PKG_AGENT_MEMORY_TOOLS_ENABLE` 都关 | -10,784 | 0 | -16,384 |
| 记忆开 + 落盘 | 在基线之上打开 `PKG_AGENT_MEMORY_PERSIST` | +1,772 | 0 | 0 |
| MQTT 工具关 | `PKG_AGENT_TOOL_MQTT_ENABLE` | -81,544 | -24 | -16,648 |
| MCP 客户端关 | `PKG_AGENT_TOOL_MCP_ENABLE` | -8,400 | 0 | 0 |
| 设备工具关 | `PKG_AGENT_TOOL_DEVICE` | -44,156 | 0 | 0 |
| 多模态与 STT 关 | `PKG_AGENT_MULTIMODAL_*` 与 `PKG_AGENT_STT_*` 切到 DISABLE | -11,028 | 0 | 0 |

记忆三种配置的绝对值：

| 配置 | text | data | bss |
|---|---|---|---|
| 记忆开，落盘关（基线） | 2,843,475 | 35,248 | 293,328 |
| 记忆关 | 2,832,691 | 35,248 | 276,944 |
| 记忆开，落盘开 | 2,845,247 | 35,248 | 293,328 |

开关变体的代码占用变化都在 text。只有 MQTT 工具关掉时 data 和 bss 跟着变，其余模块的静态 RAM 不变。

这些差值与下面按包内 `.o` 统计的表不完全一致。链接器会一起丢掉依赖代码，也会因为去重和对齐让差值变小。MQTT 工具差得最多：关掉后 paho MQTT 客户端与它引用到的网络代码一起消失，text 一共少 81,544 字节，而包内 `.o` 只有 48,809 字节。选型按这张实测表算，包内明细表用来看是哪几个文件占的。

记忆关掉后 bss 少 16,384 字节，比槽池本体的 9,024 字节多。多出来的是对齐填充：`.bss` 里的 `MMUTable` 要求 16 KB 对齐，前一段变短后它跳到下一个 16 KB 边界。真实静态 RAM 按 9,024 字节算，见第 4 节。其他行的 bss 差值也含对齐，`size` 给出的是实际预留量，按它做预算偏保守。

### 按包内文件统计

| 模块 | 字节 | 怎么关 |
|---|---|---|
| MQTT 工具 | 48,809 | `PKG_AGENT_TOOL_MQTT_ENABLE` |
| 设备框架工具 | 42,036 | `PKG_AGENT_TOOL_DEVICE` 与八个类模块开关 |
| 核心（状态机、上下文、对话） | 35,181 | 必编 |
| 协议与 JSON | 25,076 | 两份协议都编；改 SConscript 只留一份可省 7,952 |
| 内嵌技能 | 20,527 | `PKG_AGENT_SKILL_DIR` 指向只放要留的技能 |
| 通道 | 15,309 | A2A 10,965，WebSocket 4,344 |
| 分层记忆 | 10,925 | `PKG_AGENT_MEMORY_ENABLE`；只关工具省 3,940；再开落盘多 1,772 |
| 多模态与语音 | 9,683 | 关多模态后 `file_op` 与 `stt` 一起消失 |
| 工具注册与支撑 | 9,109 | 必编 |
| MCP 客户端 | 8,408 | `PKG_AGENT_TOOL_MCP_ENABLE` |
| 运算与技能工具 | 4,000 | 必编 |
| 音频驱动 | 0 | `PKG_AGENT_DRIVER_AUDIO` |

音频驱动一项是 0：当前配置编了 `AgentAudio.c`，但没有代码引用，链接时整段被丢掉，源码本身约 2.2 KB。开启播报后会回到这个量级。

MQTT 工具是最大的一块，按文件分：`tool_mqtt_ops.o` 13,718 字节、`tool_mqtt_tools.o` 9,248 字节、`tool_mqtt_client.o` 6,820 字节、`tool_mqtt_filter.o` 6,083 字节、`tool_mqtt_cmd.o` 4,892 字节、`tool_mqtt.o` 4,404 字节、`tool_mqtt_route.o` 3,612 字节。

设备框架工具里，通用层 `tool_device_core.o` 19,972 字节，I2C 6,596 字节，SPI 5,924 字节，PIN 5,100 字节，公共助手 4,316 字节。没开对应宏的模块编出来是空壳，不占固件空间。

### 记忆落盘的开销

打开 `PKG_AGENT_MEMORY_PERSIST` 后，text 增加 1,708 字节，data 与 bss 不变。这个数来自两份只差落盘宏的固件：2,848,663 与 2,846,955 字节。主配置矩阵里同一对比是 +1,772 字节（2,845,247 对 2,843,475），两份固件的基线不同，链接布局差 64 字节。落盘要求挂载可写文件系统，DFS 与 POSIX 由这个选项自动选上。

QEMU vexpress-a9 的 SD 卡上跑过两次启动。第一次写入两条记忆并落盘，14 项检查通过；第二次读回两条，12 项检查通过。文件是 `/agent_memory.dat`，纯文本，一行一条。复现步骤见 [testing.md](./testing.md) 3.8 节。

### 包里放不下时先关这些

顺序按包内字节排，括号里是该开关在完整固件上的实测 text 差值：

1. MQTT 工具，包内 48,809 字节（固件少 81,544）。
2. 不用的设备类模块，一个 5,100～6,596 字节。
3. 技能只留一份，省 16,000 字节上下。
4. 分层记忆，包内 10,925 字节（固件少 10,784）。只要不带记忆工具，关 `PKG_AGENT_MEMORY_TOOLS_ENABLE` 省 3,940。
5. 关多模态与语音转写，包内 9,683 字节（固件少 11,028）。
6. 关 MCP 客户端，包内 8,408 字节（固件少 8,400）。
7. 只编一种协议，省 7,952 字节。现在两份协议都编，Kconfig 没有单协议开关，要自己改 SConscript。

按这张表相减，关掉 MQTT、设备工具、MCP、多模态与语音，技能只留一份，包体约 97 KB。这个数是按包内字节相加，没有做组合构建实测。分层记忆再关掉，还要少 10,925 字节。

A2A 通道承载控制台连接和自发上报，关掉以后只剩 CLI 与 WebNet 通道，要确认你的产品不需要控制台。

### 更大的开销在依赖

选上本包会带 HTTPS 客户端和 TLS。当前固件里 mbedTLS 约 832,169 字节，占 text 的 29%；lwIP 约 171,285 字节；newlib 约 79,433 字节。

包本身约 224 KB，依赖的 TLS 栈 800 KB 以上。Flash 紧张的器件，先算 TLS 这一块。

## 4. 静态 RAM

Agent 包的 data 是 1,220 字节，bss 是 34,030 字节，合计 35,250 字节，约 34.4 KB。

bss 里主要是工具描述表、通道状态和协议静态表。这部分随开关增减，例如关掉 MQTT 工具后对应的静态表一起消失。

分层记忆的槽池是一块固定的静态开销：8 个核心槽加 32 个回忆槽，一个槽 200 字节（编号、长度、状态加 192 字节正文），两个槽池共 8,000 字节；再加一个 1,024 字节的注入缓冲，槽池本体共 9,024 字节。连同两个游标计数是 9,036 字节。关掉 `PKG_AGENT_MEMORY_ENABLE` 后整块下线；只改槽位数时按 200 字节一个槽增减。打开落盘不增加静态 RAM。

按 `size` 的 bss 列看，关掉记忆后 bss 少 16,384 字节，不是 9,024。多出来的是对齐填充：`.bss` 里的 `MMUTable` 要求 16 KB 对齐，前一段变短后它跳到下一个 16 KB 边界。算 RAM 预算时按 9,024 字节计。

其余模块的静态 RAM 变化看上一节的 data 与 bss 两列：关 MQTT 工具后 bss 少 16,648 字节、data 少 24 字节；关 MCP、设备工具、多模态与 STT 后 data 与 bss 都不变。开落盘也不变。

包需要 `RT_USING_HEAP`。记忆不用堆，全部走静态槽池。除了下面的运行时堆，会话上下文、技能正文和历史摘要也占堆，大小由对话长度决定。空闲堆建议至少留 12 KB 起步，再按预期最大对话长度加余量。

## 5. 运行时堆

### 5.1 单条 SSE 事件

旧实现每个事件用 cJSON 建一棵树，事件处理完释放。同一组夹具上的实测增量：

| 事件 | 堆增量（字节） | 节点数 | 字符串数 |
|---|---|---|---|
| 1 | 1,000 | 10 | 3 |
| 2 | 1,032 | 10 | 3 |
| 3 | 1,696 | 17 | 6 |
| 4 | 1,368 | 14 | 3 |
| 5 | 672 | 6 | 3 |
| 6 | 688 | 6 | 3 |

现在响应侧不建树。`json_reader.c` 一次扫描，字段到位就回调，字符串按 64 字节分片解码。同一组夹具上单事件堆增量为 0。

### 5.2 一轮对话

每轮固定的堆开销：

- 思考过程缓冲 2,048 字节，回答正文缓冲 2,048 字节，合计 4,096 字节。
- SSE 行缓冲 1,024 字节在 `chat()` 的栈上，不占堆。

出现工具调用时再加：

- 调用槽表 1,024 字节，8 项，第一次出现工具调用时才分配。纯文本轮次不占这块。
- 参数缓冲从 128 字节起步，按需倍增，上限 512 字节。
- `tool_calls` 数组在响应结束时构造，随 `chat_response_free()` 释放。

合计起来，一轮纯文本对话新增约 4.1 KB 堆，带工具调用的一轮约 5.6 KB。回归里两次运行的堆增量都是 0，清理后回到基线。

整机 `free` 快照不适合做模块开关对比。QEMU 里带 MQTT 工具时，等待窗口内 agent 线程还没起来；关掉 MQTT 后 `AgentLoop`、`agenta2a`、`fileop_wk` 已经起来，堆占用和线程集合都不同，差值不能当成模块开销。所以开关对比只报 `size` 的静态值，运行时堆按上面一轮对话的口径看。

## 6. 栈

主循环线程 `AgentLoop` 的栈是 10,240 字节。离线回归里用 `list_thread` 实测峰值占用 32%～37%，即 3,300～3,800 字节。`chat()` 的 1,024 字节行缓冲也在这个栈上。

其他线程的栈见 [framework.md](./framework.md)：文件操作线程 4,096 字节（`PKG_AGENT_FILE_OP_THREAD_SIZE`），播报线程 8,192 字节，录音线程 1,024 字节，A2A 工作线程 8,192 字节。

## 7. 怎么复现

在 BSP 工作区根目录构建，然后看 size 输出：

```bash
scons
arm-none-eabi-size rtthread.elf
```

模块明细读 `rtthread.map`。统计时排除地址为 0 的段，那些是链接器丢掉的。

开关对比用 `build/memeval/run_matrix.py`：

```bash
python build/memeval/run_matrix.py                 # 跑全部变体
python build/memeval/run_matrix.py M0_mem_on,M1_mem_off
```

脚本逐项改 `.config` 与 `rtconfig.h`，完整构建一次，读 `arm-none-eabi-size`，再跑 QEMU 抓 `free` 与 `ps`，结果写 `build/memeval/matrix.json`。跑完从 `bak/` 拷回原文件并校验 sha256，配置逐字节还原。变体定义在脚本顶部的 `VARIANTS`，基线是 `M0_mem_on`，D 系列都相对它只改一个开关。

运行时数据用离线回归：

```bash
scons -f build/skill_qemu/SConstruct
python -B build/skill_qemu/run.py
```

回归里会跑两个 utest 用例：`utest_run -thread agent.device_tools` 和 `utest_run -thread agent.memory`。前者看设备工具与技能，后者看分层记忆的槽池、检索与注入顺序。

落盘的数字来自另一份最小固件，两次启动分别写入和读回：

```bash
scons -f build/mem_persist/SConstruct
python -B build/mem_persist/run.py
```

设 `AGENT_MEMORY_PERSIST=0` 再编一次，得到不带落盘的固件，两份 size 相减就是 1,708 字节。

串口日志里搜 `MEM_PROBE` 看堆增量，搜 `AgentLoop` 看栈水位。相关探针是 `json_reader_qemu_test` 与 `stack_probe` 两个 MSH 命令。

## 8. 网页基线与实测报告

资源评估页的默认数字来自 `resource/web/client/public/agent-resource-baseline.json`。这份文件由 `scripts/resource_report.py baseline` 生成，不要手改。在 BSP 工作区根目录跑：

```bash
python scripts/resource_report.py baseline \
  --matrix build/memeval/matrix.json \
  --map build/memeval/map_M0.map
```

`--matrix` 用第 3 节的开关对比结果，`--map` 用基线变体 `M0_mem_on` 的 map。脚本按对象路径拆出包内固定体积和模块静态 RAM，再把矩阵的 text 差值填进模块 Flash。换版本、改 Kconfig 或重跑矩阵后重跑一次，基线版本号默认取当天日期。

想把某次构建的数字带进页面，用 `collect`：

```bash
python scripts/resource_report.py collect \
  --elf rtthread.elf --map rtthread.map --config rtconfig.h \
  --runtime stacks.json -o agent-report.json
```

`firmware` 来自 `arm-none-eabi-size`，`agent` 按 map 只统计 Agent 包对象，`config` 只抽白名单开关和数值，`runtime` 是可选栈测量，数组每项形如 `{"name": "AgentLoop", "bytes": 10240, "watermark": 0.31}`。白名单不含任何字符串，报告里不会出现 broker 地址、账号密码、API Key 或模型名。

也可以在资源评估页点「选择固件目录」，选带 `rtthread.elf` 与 `rtconfig.h` 的那一层。同层的 `rtthread.map` 与 `rtthread.bin` 会自动带上，也可以点「选择固件文件」逐个选。页面按同样的规则解析 ELF 节表、map 归属和 `rtconfig.h`，生成与 `collect` 同格式的报告，只存在浏览器里。固件不会上传到控制台。

页面内解析不读运行时栈，想看水位仍用 `collect --runtime` 生成报告再导入。

页面上有报告后按三项判断可信状态：模块开关、Kconfig 栈数值（文件操作、MQTT 接收、MQTT 工作）和运行时 AgentLoop 与 A2A 栈。三项都对得上标「当前配置实测」，部分对上标「同环境估算」并列出差异，没有报告标「参考估算」。校准用整机实测减去当前方案估算，只填现有固件两栏，不改模块与栈设置。
