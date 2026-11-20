# 验证方法：utest 用例与离线 QEMU 回归

> 本文说明怎么验证这个 agent 有没有按预期工作。
> 设备工具的说明见 [tool_device.md](./tool_device.md)，框架结构见 [framework.md](./framework.md)。

---

## 1. 三套验证手段

| 手段 | 位置 | 覆盖范围 | 需要硬件 |
|---|---|---|---|
| 包内 utest 用例 | `packages/Agent-latest/tests/utest_device_tools.c`、`utest_memory.c` | 通用层设备工具、模块表注册、两个技能；分层记忆的存取、检索与注入 | 否，虚拟设备 |
| 离线 QEMU 回归 | BSP 工作区 `build/skill_qemu/` | 技能、上下文压缩、设备工具、协议、MCP、端到端链路与两个 utest | 否，QEMU 模拟机 |
| 记忆落盘回归 | BSP 工作区 `build/mem_persist/` | 打开 `PKG_AGENT_MEMORY_PERSIST`，第一次启动写入，重启后读回 | 否，QEMU 模拟机 |
| 真链路 QEMU 回归 | BSP 工作区 `build/skill_qemu/run_live.py` | MCP 客户端经网口连到 PC 上的网关，调桌面工具和联网搜索 | 否，但要 PC 上有网关 |

这几套都在 QEMU 或板子上用 MSH 命令触发，不需要手工点界面。

---

## 2. 包内 utest 用例

### 2.1 编译条件

包内 `SConscript` 只在 `RT_USING_UTEST` 打开时编译 `tests/` 目录：

```python
SRC_TEST = os.path.join(CWD, "tests")
if GetDepend(["RT_USING_UTEST"]) and os.path.isdir(SRC_TEST):
    for filename in os.listdir(SRC_TEST):
        ...
```

当前 BSP 的 `.config` 里 `CONFIG_RT_USING_UTEST` 没有打开，所以常规 `scons` 不会编这个用例。要在板子上跑它，先在 menuconfig 里打开 `RT_USING_UTEST`。

### 2.2 运行方法

用例文件是 `packages/Agent-latest/tests/utest_device_tools.c` 与 `utest_memory.c`，导出名分别是 `agent.device_tools` 和 `agent.memory`：

```c
UTEST_TC_EXPORT(testcase, "agent.device_tools", utest_tc_init, utest_tc_cleanup, 60);
UTEST_TC_EXPORT(testcase, "agent.memory", utest_tc_init, utest_tc_cleanup, 60);
```

最后一个参数 60 是超时秒数。运行命令：

```
msh /> utest_run -thread agent.device_tools
msh /> utest_run -thread agent.memory
```

`-thread` 表示用例在自己的线程里跑。成功时输出：

```
[  PASSED  ] [ result   ] testcase (agent.device_tools)
[  PASSED  ] [ result   ] testcase (agent.memory)
```

设备用例自己注册三个虚拟设备：`utst0`（字符）、`utblk0`（块）、`utrtc0`（RTC）。同时用真实的 `uart0` 检查控制台保护。`utest_tc_init` 调用 `init_tools()` 注册工具，`utest_tc_cleanup` 把引用计数清零。注册检查遍历的是通用模块表，接口是 `agent_tool_module_count()` 这一组，设备模块和运算、技能、MQTT 模块一起核对。

### 2.3 九个单元组

用例按九个单元组组织，每组用 `UTEST_UNIT_RUN` 触发。

| 单元 | 检查内容 |
|---|---|
| `unit_tool_registration` | 遍历模块表，模块声明的每个工具都能在工具链表查到；模块数为 0 或越界查询被判失败 |
| `unit_list_and_info` | 列表带出 `uart0`、`utst0`、`utblk0`、`utrtc0`；类过滤；类名写错被拒；`device_info` 报类号、引用计数、驱动操作、控制台标记 |
| `unit_lifecycle_refs` | 未打开就读写被拒；打开后引用计数变 1；只读句柄上写被拒；关闭后计数回 0；再关一次被拒；`mode` 与 `flags` 组合生效 |
| `unit_char_io` | 文本写读回环；hex 写读回环；hex 里有非十六进制字符被拒；空数据被拒；读到 0 字节被解释 |
| `unit_block_io` | `BLK_GETGEOME` 返回几何；整块写、整块读；非整块写被拒；超量读被截断到 2048 字节传输上限 |
| `unit_control_commands` | 数字命令号成功与失败；给无参命令传 `arg` 被拒；结构体参数命令被拒；未知命令名被拒；`RTC_SET_TIME` 缺 `arg` 被拒；`RTC_GET_TIME` 与 `RTC_SET_TIME` 传值正确 |
| `unit_console_guards` | 读控制台被拒；关控制台被拒 |
| `unit_error_paths` | 设备不存在、空设备名、非法打开标志、缺 `cmd` |
| `unit_skill_catalog` | 技能表里有 `device` 与 `device-commands`；`skill` 工具返回的正文与内嵌注册表逐字节相同 |

### 2.4 分层记忆用例

`agent.memory` 不依赖外设，只操作静态槽池。五个单元组：

| 单元 | 检查内容 |
|---|---|
| `unit_store_and_dedup` | 空池时条数为 0、注入返回空；带首尾空白的文本存进去被压缩；重复写入相同文本不增加条数；空白、空指针和非法层号返回 `-RT_EINVAL` |
| `unit_recall_query` | 英文关键词命中 1 条并带 `[id=` 前缀；中文词“温度”命中 1 条；不传关键词按新到旧列出；无关查询返回 0 条且缓冲为空；`agent_memory_list` 列出全部 3 条 |
| `unit_eviction` | 写满容量再写一条，条数停在容量；core 和 recall 两个环各自覆盖最旧的一条，最新一条排在最前 |
| `unit_delete` | 按 id 删除后条数减一、槽位读不到；同一 id 再删返回 `-RT_ENOENT`；另一层不受影响；空出的槽位可以继续写入 |
| `unit_inject_order` | 注入块带标题行、core 与 recall 正文；游标顺序是 system 覆盖文本、注入块作为 user 消息、最后才是本轮用户输入 |

用例在初始化与清理里调用 `agent_memory_init()` 与 `agent_memory_deinit()`。默认配置关闭落盘，所以清理不会写文件；落盘行为单独用 3.8 节的两次启动回归验证。

---

## 3. 离线 QEMU 回归

回归工程放在 BSP 工作区的 `build/skill_qemu/`，不在 Agent 包内。它用同一套 BSP 配置，另加测试源文件，生成一份测试固件。

### 3.1 目录内容

| 文件 | 作用 |
|---|---|
| `SConstruct` | 复用 BSP 的 `rtconfig.py` 与根 `SConstruct`，额外编入测试文件，把产物输出到 `build/skill_qemu/rtthread.elf` 与 `rtthread.bin` |
| `run.py` | 启动 QEMU，走串口发命令，收集结果，写 `result.json`，最后校验并退出 QEMU |
| `run_live.py` | 真链路回归：起文件服务器、搜索服务替身与主机侧网关，起带网卡的 QEMU，跑 `mcp_live_qemu_test`，写 `result_live.json` |
| `search_stub.py` | 本地的搜索服务替身，形状是 SearXNG 的 JSON 接口，返回里带一个固定标记 |
| `mock_chat.c` / `mock_chat.h` | 在 webclient 边界替换模型，同时拦住 MCP 端点上的请求。客户端分两段发请求，替身也拦 `webclient_write` 与 `webclient_handle_response` |
| `test_result.h` | 结果行的打印助手，保证判据不被别的线程日志切断 |
| `test_skill.c` | 技能与目录的集成检查 |
| `test_context.c` | 上下文压缩与 SSE 解析检查 |
| `test_device.c` | 设备框架工具的集成检查 |
| `test_anthropic.c` | 协议切换与 Anthropic 报文格式检查 |
| `test_agent_e2e.c` | 端到端链路检查 |
| `test_mcp.c` | MCP 客户端的离线检查 |
| `test_mcp_live.c` | MCP 客户端的真链路检查 |
| `test_json_reader.c` | 流式 JSON 读取器检查，另导出 `stack_probe` 打印线程栈水位 |
| `run_llm_live.py` | 模型真链路：板子经网卡直接连 DeepSeek |

`SConstruct` 里的关键设置：

- 链接参数加 `-Wl,--wrap=webclient_post,--wrap=webclient_read`，把模型换掉。
- `UTEST_THR_STACK_SIZE` 提到 16384，`RT_UTEST_MAX_OPTIONS` 设为 64。
- 额外编入 `rt-thread/components/utilities/utest/utest.c`、`packages/Agent-latest/tests/utest_device_tools.c` 与 `utest_memory.c`。

### 3.2 模型替换方式

`mock_chat.c` 用 `--wrap` 接管 `webclient_post()` 与 `webclient_read()`。会话建立、请求头、超时设置、`chat()` 与 SSE 解析都走真实代码，只有发出去的请求和读回来的字节被换成固定夹具。

夹具按顺序取用，测试代码通过 `mock_chat_add_response()` 预置 SSE 文本，`mock_chat_enable()` 打开拦截。抓到的请求可以用 `mock_chat_request()` 取出来检查。

代码里明确要求不要打印抓到的报文、URI、模型名和鉴权头。

### 3.3 构建命令

在 BSP 工作区根目录执行：

```bash
scons -f build/skill_qemu/SConstruct
```

产物是 `build/skill_qemu/rtthread.elf` 和 `build/skill_qemu/rtthread.bin`。

测试固件用的是这个 BSP 的配置，`RT_USING_PWM`、`RT_USING_ADC`、`RT_USING_DAC`、`RT_USING_SENSOR`、`RT_USING_LED` 都没开，五个模块不参与链接。要单独检查八个模块能不能编译，用同一套编译参数走一遍语法检查：

```bash
AGENT_DEV_SYNTAX=1 scons -f build/skill_qemu/SConstruct build/skill_qemu/syntax/
```

`SConstruct` 里的 `syntax_check_modules()` 会把缺失的 `RT_USING_*` 与 `PKG_AGENT_TOOL_DEVICE_*` 补成命令行宏，逐个编译 `src/tools/tool_device/` 下的 10 个 `.c`，产物落在 `build/skill_qemu/syntax/`，不做链接。它只证明代码在各配置分支下能编过，不证明运行时行为。

### 3.4 QEMU 启动方式

`run.py` 自己拉起 QEMU，参数如下：

```bash
qemu-system-arm -M vexpress-a9 -smp cpus=2 \
  -kernel build/skill_qemu/rtthread.bin \
  -drive file=sd.bin,if=sd,format=raw,snapshot=on \
  -chardev socket,id=serial0,host=127.0.0.1,port=<随机端口> \
  -serial chardev:serial0 -monitor none -display none \
  -audiodev none,id=audio0 -net none \
  -qmp tcp:127.0.0.1,<随机端口>,server=on,wait=off
```

串口走 TCP，脚本按字符逐个发命令。QEMU 可执行文件路径写在 `run.py` 的 `QEMU` 常量里，按本机实际位置改。

### 3.5 命令序列

脚本等系统起来后，依次执行这十一条命令，每条都要等到串口输出对应的结果行：

| 命令 | 结果行 | 覆盖内容 |
|---|---|---|
| `agent_protocol openai` | `agent protocol: openai` | 把协议钉到 OpenAI，后面大部分测试的夹具都是 OpenAI 格式 |
| `skill_qemu_test` | `SKILL_QEMU RESULT checks=<n> failures=<n>` | 技能注册表、`skill` 工具、目录 JSON、技能正文进上下文、重复调用不涨堆 |
| `context_qemu_test` | `CONTEXT_QEMU RESULT checks=<n> failures=<n>` | 上下文压缩、摘要请求与校验、SSE 夹具解析 |
| `device_qemu_test` | `DEVICE_QEMU RESULT checks=<n> failures=<n>` | 通用层 7 个工具在真实框架上的行为，外加模块表与工具链表的一致性、每个工具都带上非空 description |
| `mcp_qemu_test` | `MCP_QEMU RESULT checks=<n> failures=<n>` | MCP 握手三步、远端工具注册与名字替换、`tools/call` 回包解析、会话 ID 回带、服务端不可达时的失败提示与重连 |
| `agent_e2e_test` | `AGENT_E2E RESULT checks=<n> failures=<n>` | 从 Debug 通道收消息，模型发起工具调用，工具真的读写外设，结果回到模型 |
| `stack_probe` | `STACK_PROBE_DONE` | 打印各线程栈水位，做占用基线 |
| `protocol_qemu_test` | `PROTOCOL_QEMU RESULT checks=<n> failures=<n>` | 协议切换、Anthropic 请求体形态、鉴权头、事件式 SSE 解析、工具调用与工具结果的报文翻译 |
| `json_reader_qemu_test` | `JSON_READER_QEMU RESULT checks=<n> failures=<n>` | 流式 JSON 读取器与协议字段提取 |
| `utest_run -thread agent.device_tools` | `[  PASSED  ] [ result   ] testcase (agent.device_tools)` | 包内设备工具用例 |
| `utest_run -thread agent.memory` | `[  PASSED  ] [ result   ] testcase (agent.memory)` | 包内分层记忆用例 |

两个包内用例也在两轮里各跑一次。脚本除了数 PASSED，还要数 `[  ASSERT  ]` 行。这个内核的 utest 在每个单元组开始时清零失败计数（`rt-thread/components/utilities/utest/utest.c` 里的 `utest_unit_run()`），所以某个单元组里的断言失败可能被后面的单元组覆盖，最终照样打印 PASSED。脚本要求断言行数为 0。

第一条 `agent_protocol openai` 是必需的。固件的默认协议由 `PKG_AGENT_API_PROTOCOL_SEL` 决定，夹具是 OpenAI 格式的 `data: {"choices":[...]}`。默认协议是 Anthropic 时，解析器认不出这些夹具，上下文压缩和端到端两轮会失败。脚本先用这条命令把协议钉成 OpenAI，回归结果就跟固件默认配置无关了。`protocol_qemu_test` 自己会切到 Anthropic 再切回来，所以不影响它。

### 3.6 两次运行协议

每条命令都跑两遍。理由是单次通过可能掩盖状态残留：第二遍在第一遍留下的上下文和堆上再跑一次，堆差值应该还是 0。

每个测试还会打印一行堆快照：

```
SKILL_QEMU HEAP before=<字节> after=<字节> delta=<差值>
```

脚本最后要求所有堆快照的 `delta` 都是 0。

### 3.7 结果文件

脚本把结果写成 `build/skill_qemu/result.json`：

| 字段 | 含义 |
|---|---|
| `machine` | QEMU 机型，这里是 `vexpress-a9` |
| `firmware` | 测试固件路径 |
| `results` | `skill_qemu_test` 两轮的检查数与失败数 |
| `heap` | `skill_qemu_test` 两轮的堆快照 |
| `context_results` / `context_heap` | `context_qemu_test` 同上 |
| `device_results` / `device_heap` | `device_qemu_test` 同上 |
| `agent_e2e_results` | `agent_e2e_test` 两轮的检查数与失败数 |
| `agent_e2e_answers` | 端到端测试收到最终回答的次数 |
| `protocol_results` / `protocol_heap` | `protocol_qemu_test` 两轮的检查数、失败数与堆快照 |
| `mcp_results` | `mcp_qemu_test` 两轮的检查数与失败数 |
| `json_reader_results` | `json_reader_qemu_test` 两轮的检查数与失败数 |
| `utest` | `agent.device_tools` 用例的 `name`、`passed_runs`、`failed_runs`、`assert_failures` |
| `utest_memory` | `agent.memory` 用例的同样四项 |
| `network_enabled` | 固定为 `false`，QEMU 用了 `-net none` |
| `disk_snapshot` | 固定为 `true`，SD 卡挂了 `snapshot=on` |
| `summary_transport` | 固定为 `deterministic SSE fixtures at webclient post/read boundary` |

脚本退出前会自己判一遍，判据分四档：包内用例两轮都 PASSED、0 FAILED、0 断言行；`agent_e2e_test` 两轮都有最终回答，每轮 `checks` 不少于 5；技能、上下文、设备三组每轮 `checks` 不少于 20；协议组与 MCP 组每轮 `checks` 不少于 10。五组结果和四个堆快照数组的长度都必须是 2，缺一条就判定失败。所有堆快照的 `delta` 必须为 0。任何一条不满足就抛异常，进程以非 0 退出。

结果行会被别的线程的日志切断。`rt_kprintf` 逐字符写串口，本工程没开 `RT_USING_THREADSAFE_PRINTF`，MQTT 线程中途插一句，`MCP_QEMU RESULT checks=.. failures=..` 就断成两截，脚本匹配不到而误报超时。所以所有结果行统一走 `test_result.h` 里的 `test_print_result()`，它在 `rt_enter_critical()` 里打印整行。

### 3.8 记忆落盘回归

主回归固件不开 `PKG_AGENT_MEMORY_PERSIST`。打开它以后，`agent.memory` 用例会读到上一次留下的数据，单元组之间互相干扰。主回归的 SD 卡又挂了 `snapshot=on`，写下去的内容重启就没了。

落盘单独做一个最小固件，放在 `build/mem_persist/`：

| 文件 | 作用 |
|---|---|
| `SConstruct` | 包一层 `PrepareBuilding`，在它开始编包之前把 `PKG_AGENT_MEMORY_PERSIST=1` 与 `PKG_AGENT_MEMORY_FILE="/agent_memory.dat"` 补进 env，不改 BSP 的 `rtconfig.h`，产物是 `rtthread.elf` 与 `rtthread.bin` |
| `test_memory_persist.c` | 导出 `mem_persist_write` 与 `mem_persist_read` 两条 MSH 命令 |
| `run.py` | 启动两次 QEMU，收集结果，写 `result.json` |

构建和运行：

```bash
scons -f build/mem_persist/SConstruct
python -B build/mem_persist/run.py
```

设 `AGENT_MEMORY_PERSIST=0` 再编一次，产物名带 `-off` 后缀，用来对比不落盘时的体积。

`run.py` 先把 BSP 根目录的 `sd.bin` 复制到 `build/mem_persist/sd.bin`，挂盘参数是 `cache=writethrough`，不带 `snapshot=on`。这样第一次启动写下去的文件，第二次启动还在。

两次启动各跑一条命令：

| 启动 | 命令 | 结果行 |
|---|---|---|
| 第一次 | `mem_persist_write` | `MEM_PERSIST_WRITE RESULT checks=<n> failures=<n>` |
| 第二次 | `mem_persist_read` | `MEM_PERSIST_READ RESULT checks=<n> failures=<n>` |

第一轮做的事：删掉旧文件，初始化空池，写入一条 core 和一条 recall，检查条数、编号和注入文本，然后 `agent_memory_deinit()` 落盘，检查文件存在、正文正确、每行以 `c ` 或 `r ` 开头。第二轮是新进程：检查文件还在，`agent_memory_init()` 读回两条，核对正文和编号，按关键词查一次，删掉两条再落盘，最后文件长度为 0。

当前结果：

```
MEM_PERSIST_WRITE RESULT checks=14 failures=0
MEM_PERSIST_READ RESULT checks=12 failures=0
```

串口日志能直接看到两次落盘：

```
[I/Agent.memory] Saved 2 memory items to /agent_memory.dat
[I/Agent.memory] Loaded 2 memory items from /agent_memory.dat
```

落盘代码占 1,708 字节 Flash，`.data` 与 `.bss` 不变。`result.json` 里记着 `boots`、`persist_file`、`write`、`read` 四个字段。

存盘先写 `*.tmp` 再改名。elmfat 的 `f_rename` 遇到同名目标返回 `FR_EXIST`，所以改名失败时先删旧文件再试一次。加载时主文件打不开会去读 `.tmp`，掉电停在改名之前也能恢复。改名回退这条路径第二轮写盘时跑到了；`.tmp` 恢复要构造第一次改名后掉电，回归里没有覆盖。

---

## 4. 真链路 QEMU 回归

离线回归全程用替身，不出网。真链路回归让板子经 QEMU 的网卡连到 PC 上的网关，跑一遍真的 MCP 握手和桌面工具调用。

```bash
python build/skill_qemu/run_live.py
```

脚本做五件事：

1. 起文件服务器，端口 8000，用的是 `packages/Agent-latest/resource/file_server/.venv` 里的 Python。工具回包里的图片存在这里。
2. 起本地的搜索服务替身，端口 8799，形状是 SearXNG 的 JSON 接口。联网搜索的判据靠它，替身的返回里带一个固定的标记。
3. 起主机侧网关，参数是 `--windows-mcp --host 0.0.0.0 --port 8765 --root <用户目录> --file-server http://127.0.0.1:8000 --web-search-url http://127.0.0.1:8799/search --web-search-mode searxng`。网关另一头是 Windows-MCP，端口能连上说明它已经握手完。
4. 起 QEMU，带 TAP 网卡：`-net nic,model=lan9118,netdev=net0 -netdev tap,id=net0,ifname=qemu`。板子接 Windows 共享出来的那张 TAP 网卡，固定地址 `192.168.137.100`，宿主在共享网段上是 `192.168.137.1`。
5. 在板子上跑 `mcp_live_qemu_test`，结果写进 `build/skill_qemu/result_live.json`。

固件的 `PKG_AGENT_MCP_SERVER_URL` 要配成 `http://192.168.137.1:8765/mcp`。走 TAP 网桥时宿主地址是 `192.168.137.1`，不是 PC 的真实 IP。开了自动获取地址就以控制台报的为准，这条回归要对着控制台回包里的 `mcpUrl` 核对。

离线回归启动时 `mock_chat` 会拦下这个地址上的所有 MCP 请求。`mcp_live_qemu_test` 第一步调 `mock_mcp_release()` 放行，后面的握手、取工具表、调桌面工具都走真链路。

板上的 22 项检查：

| 检查 | 判据 |
|---|---|
| 替身放行 | 放行标志生效 |
| 握手成功 | 与真网关完成 `initialize` |
| 工具表拿到 | `tools/list` 返回数组 |
| 工具表规模 | 不少于 20 项，实测 35 项 |
| 桌面工具在表里 | 有 `win__` 前缀的工具 |
| 文件工具在表里 | 有 `read_text_file` |
| 桌面工具调用成功 | `win__displayinventory` 返回成功 |
| 桌面工具回真信息 | 回包里含 `DISPLAY` |
| 进程工具调用成功 | `win__process` 返回成功 |
| 进程工具回真信息 | 回包里含 `PID` |
| 截图回包收得下 | `win__screenshot` 回成一句说明 |
| 截图存进文件服务器 | 说明里有 `image saved` |
| 截图有下载地址 | 说明里有 `/download/` |
| 音频回包收得下 | `user__audio` 返回成功 |
| 音频渲染成条目 | 板端回包里含 `[audio:` |
| 音频有下载地址 | 条目里有 `/download/` |
| 搜索调用成功 | `web_search` 返回成功 |
| 搜索回真结果 | 回包里含替身的标记 |
| 桌面结构回包收得下 | `win__snapshot` 回得出文本 |
| 应用启动成功 | `win__app` 把 QQ 启动起来 |
| 启动工具回了话 | 回包里含启动结果 |

结果行是 `MCP_LIVE RESULT checks=<n> failures=<n>`。22 项全过才算绿。

最后一条会在跑测试的 PC 上真的打开 QQ。它走的是和用户发指令一样的路径：板子发工具
调用，网关转给 Windows-MCP，Windows-MCP 按开始菜单里的名字启动它。

脚本最后按板端拿到的地址把图取回来，确认存下来的确实是 PNG，字节数记在
`result_live.json` 的 `stored_image` 里。这条证明的是图片真的落了盘、板端拿到的
地址能打开；它不证明模型会读这个地址。

这套回归证明的是链路通：板子能经网口连到 PC 上的网关，握手、取工具表、调工具、收结果都正常。它不证明模型会自己选对桌面工具，也不证明 Windows-MCP 的每个工具都好用。

### 4.1 模型真链路

MCP 那条链路走的是板子到 PC 网关，模型流量还是替身。要让请求真的打到 DeepSeek，用另一条脚本：

```bash
python build/skill_qemu/run_llm_live.py
```

这条脚本不需要网关。板子经 QEMU 的网卡直接出网，请求地址是 `https://api.deepseek.com/anthropic/v1/messages`。

板上的四项检查：

| 检查 | 判据 |
|---|---|
| 替身放行 | 替身不再应答，请求真的出网 |
| 真实端点有回包 | 请求发到 DeepSeek 并拿到应答 |
| 回包带正文 | 应答里有文本 |
| 没有工具调用 | 应答没有工具调用 |

结果行是 `LLM_LIVE RESULT checks=4 failures=0`。

两处前提：

- **板端时钟要对。** DeepSeek 证书的 notBefore 是 2026-09-03，软 RTC 上电停在 2018-01-01，握手会因「证书有效期在未来」失败，报 -0x2700。用例开头会把系统时间设到当前时间附近。
- **这条用例不能挂在 MSH 线程上跑。** MSH 线程的栈是 `FINSH_THREAD_STACK_SIZE`，默认 4KB。TLS 握手加熵收集的栈用量远超这个数，硬跑会把栈冲垮，踩坏相邻的堆块。症状是 `data abort`，落在 `rt_list_remove` 里读野指针。用例自己起一个 32KB 的线程跑检查，MSH 线程只负责等结果。

---

## 5. 这套回归不能证明什么

回归覆盖了框架逻辑，但有六条硬边界。

- **模型是假的。** `mock_chat.c` 把模型换成固定 SSE 夹具。请求怎么写、参数怎么选、总结写得好不好，真实模型的行为没有被验证。端到端测试里的工具调用是夹具写死的，不是模型自己决定的。真正打到 DeepSeek 的只有 `run_llm_live.py` 那一条，它只发一次普通提问，不带工具，所以模型挑工具的能力仍然没有被验证。
- **夹具只有 OpenAI 和 Anthropic 文本流两种。** 上下文压缩、端到端和网络回归用的是 OpenAI 格式的 SSE 夹具，回归开始前用 `agent_protocol openai` 把协议钉死。`test_context.c` 里另有一条 Anthropic 文本流的压缩用例，验证事件式 SSE 能产出摘要并替换历史。Anthropic 的工具调用与多轮工具循环仍只在 `protocol_qemu_test` 里验证请求体形态、鉴权头和报文翻译。`protocol_qemu_test` 自己会切到 Anthropic 再切回来。
- **外设是假的。** `utst0`、`utblk0`、`utrtc0` 以及端到端用的虚拟设备都是内存里的桩。真实驱动的时序、中断、DMA、掉电行为没有被覆盖。块设备不会真的擦写介质。
- **环境是隔离的。** 离线回归的网络是关的，SD 卡是快照。MQTT、TTS、文件上传这类需要外网的路径在离线回归里跑不到。真链路回归只开了 MCP 一条路，MQTT 和 TTS 仍不参与。
- **落盘只在 QEMU 的 SD 卡上验证过。** 落盘回归用 FAT 文件系统加 `cache=writethrough`，验证的是正常重启。写 `*.tmp` 与改名之间掉电的窗口没有构造。真实介质（SPI Flash、eMMC）的擦写时序、坏块和磨损也没有覆盖。主回归仍是 `snapshot=on`，跑完不留记忆文件。
- **配置是这份 BSP 的。** 固件里只有通用层和 GPIO、I2C、SPI 四个模块。PWM、ADC、DAC、传感器、LED 五个模块只做了编译检查，运行时行为要等到有对应驱动的板子上再看。模块表那条检查遍历的是当前编译进来的模块，不会因为某个模块缺席而报错。

另外，回归固件里的包内用例是 `SConstruct` 显式加进去的，不依赖 BSP 的 `RT_USING_UTEST` 配置。

---

## 6. 真机验证

板子上跑同一套检查，用 Debug 通道驱动对话。

### 6.1 通道选择

`agent_send_message` 命令属于 Debug 通道。验证前把 `.config` 里的通道换成 Debug，同步生成 `rtconfig.h` 后再编译。生成配置时保留已有 `PKG_*_PATH` 字符串，验证完恢复原通道。TCP 通道测试使用独立脚本，见第 8 节。LVGL 通道的实现由应用工程提供。

### 6.2 手动跑包内用例

打开 `RT_USING_UTEST` 后重新编译烧录，然后在串口执行：

```
msh /> utest_run -thread agent.device_tools
msh /> utest_run -thread agent.memory
```

### 6.3 用对话验证设备工具

Debug 通道提供 MSH 命令 `agent_send_message <type> <content>`。`type` 取 0 到 4，0 是文本。命令把消息投给 agent，然后等输出，最多等 30 秒。

```
msh /> main_loop_entry
msh /> agent_send_message 0 列出所有设备
msh /> agent_send_message 0 打开串口 2 并读 16 字节
msh /> agent_send_message 0 读一下 RTC 的时间
```

对话里要验的行为：

1. 模型先调 `device_list`，再用返回的名字调其它工具，不自己编设备名。
2. 需要命令目录时，模型先调 `skill` 加载 `device-commands`。
3. 写操作前模型会先问用户要授权。
4. 读控制台和关控制台被拒时，模型如实报告原因，不换个工具硬试。
5. 引用计数守恒：开两次关两次，`device_info` 里 `ref_count` 回到 0。

### 6.4 查日志

串口日志能看出工具调用的完整过程：每次调用的工具名、参数和返回文本。`[mem] cleanup done` 的 `used` 用来判断有没有泄漏，反复执行「进入对话 → 清理 → 再进入」看这个数是否持平。

## 7. 外部服务器地址回归

地址来源有两种模式，各跑一遍。

手动模式三个地址都在 Kconfig 里填死，板端启动时不发任何请求。自动模式只填控制台地址，启动时向控制台问一次，问不到就用固件内置的地址。

```bash
python build/qemu_endpoint_check.py manual
python build/qemu_endpoint_check.py auto
```

脚本在 BSP 工作区里，不在包内。它做三件事：

1. 起一个假控制台，端口取固件的 `PKG_AGENT_CONSOLE_URL`，`GET /api/ops/endpoints` 回一份地址跟 Kconfig 不一样的 JSON，报的端口是 8100 与 8790。
2. 起 QEMU，等板子启动完，打 `msh_agent_endpoint` 看板子用的是哪一份地址。
3. 自动模式下再起一次板子，这次假控制台不答，板子应该回退到固件内置的地址。

判据一共 10 项。手动模式：模式是 manual，控制台地址、文件服务器地址、MCP 地址都是 Kconfig 里的值，串口日志里没有 `[endpoint] GET`。自动模式加控制台在：模式是 auto，地址是假控制台报的那两个，日志里有 `[endpoint] file server ..., mcp ...`，没有回退告警。自动模式加控制台不在：日志里有 `[endpoint] console unreachable`，地址回落到固件内置的那两个。

脚本从固件的 `rtconfig.h` 读 `PKG_AGENT_CONSOLE_URL`，假控制台就起在这个地址的端口上。回退地址先读 `rtconfig.h`，自动模式下这两个宏不在 `rtconfig.h` 里，改读包内 `inc/utils/agent_endpoint.h` 的默认值。改了固件配置不用改脚本。

## 8. WebSocket 实际联调

先编译固件、Spring 后端与 Vue 前端，再运行 `scripts/mqtt_test/websocket_e2e_test.py`。
脚本使用当前 8080 后端和实际数据库，默认新增独立测试账号，不修改原设备数据。
要落到已有账号就用 `CONSOLE_USER=... CONSOLE_PASSWORD=...` 指定；加 `--keep` 结束后保留 QEMU 与设备，
供浏览器查看。QEMU 使用 SD 副本和 user 网络，串口 5556，不占用户现有 TAP 与 5555 终端。

要连独立后端（不碰日常设备）就同时设三个环境变量：

```
CONSOLE_URL=http://127.0.0.1:18080 QEMU_BOARD_PORT=18080 \
AGENT_CONSOLE_DB=<独立后端的库路径> python websocket_e2e_test.py
```

`CONSOLE_URL` 是脚本自己调 API 与建 WebSocket 的地址，`QEMU_BOARD_PORT` 是板端
`a2a_activate` 连的端口，`AGENT_CONSOLE_DB` 是脚本清理测试账号时删库的路径。三者指向同一份后端，
少设一个就会出现"脚本连对了、板子连错服务器"或"清理落到主库"。

控制台重启这一项要外部配合停/起后端，把整条命令放进 `CONSOLE_RESTART_CMD`；
不设就打印 SKIP 跳过。板端重启重连默认执行，`--no-board-restart` 可跳过。

判据分五组：

1. 激活前用错密码连一次，必须被拒 `bad_credential`。
2. 板端 `a2a_activate` 拿到一次性 token 并长连；Web 状态从激活中变在线，连接窗口显示已连接。
3. 探活返回的是 welcome 之后随连接推下去的绑定能力卡，技能与 `pushNotifications` 都在；再经 Web 发任务收到非空 final。
4. 板端重启后凭 SD 卡里的 token 自动重连、设备回到在线。QEMU 被突然杀掉时，服务端还挂着旧连接，
   设备 ID 会一直报 `already_online`，最长要等心跳超时 `timeout + 30 s`（约 50 s）才回收；
   板端每 3 秒重试一次，脚本按 95 秒等。控制台重启后同样要自己连回来。
5. 两次复检：已连接的设备再拿密码连一次必须被拒 `already_online`；另一台设备第二次激活必须被拒 `already_activated`。

日志及任务回复在 ignored 的 `resource/web/server/target/ws-test/`。
LLM 连接失败时必须单列说明，不能把能力卡或连接状态算作计算任务成功。

踩过的坑：`a2a_activate` 跑在 tShell（默认 4096 栈）里，`agent_ws_upgrade` 曾在栈上开 2 KB 响应缓冲，
把 shell 线程撑爆。溢出后 `rt_schedule` 里的栈检查要打日志，又在调度器内部拿 ulog 互斥锁，直接撞
`RT_DEBUG_SCHEDULER_AVAILABLE` 断言，串口只看得到一行断言、看不到是哪个线程。已把响应缓冲降到 768 字节，
`FINSH_THREAD_STACK_SIZE` 提到 8192；升级响应头只有两百字节，缓冲写满返回 -1 而不是越界。

