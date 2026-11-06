# 验证方法：utest 用例与离线 QEMU 回归

> 本文说明怎么验证这个 agent 有没有按预期工作。
> 设备工具的说明见 [tool_device.md](./tool_device.md)，框架结构见 [framework.md](./framework.md)。

---

## 1. 两套验证手段

| 手段 | 位置 | 覆盖范围 | 需要硬件 |
|---|---|---|---|
| 包内 utest 用例 | `packages/Agent-latest/tests/device_tools_tc.c` | 通用层设备工具、模块表注册、两个技能，逐条断言 | 否，虚拟设备 |
| 离线 QEMU 回归 | BSP 工作区 `build/skill_qemu/` | 技能、上下文压缩、设备工具、端到端链路 | 否，QEMU 模拟机 |

两套都在 QEMU 或板子上用 MSH 命令触发，不需要手工点界面。

---

## 2. 包内 utest 用例

### 2.1 编译条件

包内 `SConscript` 只在 `RT_USING_UTEST` 打开时编译 `tests/` 目录：

```python
SRC_TEST = os.path.join(CWD, "test")
if GetDepend(["RT_USING_UTEST"]) and os.path.isdir(SRC_TEST):
    for filename in os.listdir(SRC_TEST):
        ...
```

当前 BSP 的 `.config` 里 `CONFIG_RT_USING_UTEST` 没有打开，所以常规 `scons` 不会编这个用例。要在板子上跑它，先在 menuconfig 里打开 `RT_USING_UTEST`。

### 2.2 运行方法

用例文件 `packages/Agent-latest/tests/device_tools_tc.c`，导出名是 `agent.device_tools`：

```c
UTEST_TC_EXPORT(testcase, "agent.device_tools", utest_tc_init, utest_tc_cleanup, 60);
```

最后一个参数 60 是超时秒数。运行命令：

```
msh /> utest_run -thread agent.device_tools
```

`-thread` 表示用例在自己的线程里跑。成功时输出：

```
[  PASSED  ] [ result   ] testcase (agent.device_tools)
```

用例自己注册三个虚拟设备：`utst0`（字符）、`utblk0`（块）、`utrtc0`（RTC）。同时用真实的 `uart0` 检查控制台保护。`utest_tc_init` 调用 `init_tools()` 注册工具，`utest_tc_cleanup` 把引用计数清零。注册检查遍历的是通用模块表，接口是 `agent_tool_module_count()` 这一组，设备模块和运算、技能、MQTT 模块一起核对。

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

---

## 3. 离线 QEMU 回归

回归工程放在 BSP 工作区的 `build/skill_qemu/`，不在 Agent 包内。它用同一套 BSP 配置，另加测试源文件，生成一份测试固件。

### 3.1 目录内容

| 文件 | 作用 |
|---|---|
| `SConstruct` | 复用 BSP 的 `rtconfig.py` 与根 `SConstruct`，额外编入测试文件，把产物输出到 `build/skill_qemu/rtthread.elf` 与 `rtthread.bin` |
| `run.py` | 启动 QEMU，走串口发命令，收集结果，写 `result.json`，最后校验并退出 QEMU |
| `mock_chat.c` / `mock_chat.h` | 在 webclient 边界替换模型 |
| `test_skill.c` | 技能与目录的集成检查 |
| `test_context.c` | 上下文压缩与 SSE 解析检查 |
| `test_device.c` | 设备框架工具的集成检查 |
| `test_agent_e2e.c` | 端到端链路检查 |

`SConstruct` 里的关键设置：

- 链接参数加 `-Wl,--wrap=webclient_post,--wrap=webclient_read`，把模型换掉。
- `UTEST_THR_STACK_SIZE` 提到 16384，`RT_UTEST_MAX_OPTIONS` 设为 64。
- 额外编入 `rt-thread/components/utilities/utest/utest.c` 与 `packages/Agent-latest/tests/device_tools_tc.c`。

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

### 3.5 四个命令

脚本等系统起来后，依次执行这四条 MSH 命令，每条都要等到串口输出对应的结果行：

| 命令 | 结果行 | 覆盖内容 |
|---|---|---|
| `skill_qemu_test` | `SKILL_QEMU RESULT checks=<n> failures=<n>` | 技能注册表、`skill` 工具、目录 JSON、技能正文进上下文、重复调用不涨堆 |
| `context_qemu_test` | `CONTEXT_QEMU RESULT checks=<n> failures=<n>` | 上下文压缩、摘要请求与校验、SSE 夹具解析 |
| `device_qemu_test` | `DEVICE_QEMU RESULT checks=<n> failures=<n>` | 通用层 7 个工具在真实框架上的行为，外加模块表与工具链表的一致性、每个工具都带上非空 description |
| `agent_e2e_test` | `AGENT_E2E RESULT checks=<n> failures=<n>` | 从 Debug 通道收消息，模型发起工具调用，工具真的读写外设，结果回到模型 |

同一条序列里还有包内用例，它也在两轮里各跑一次：

```
utest_run -thread agent.device_tools
```

匹配的结果行是 `[  PASSED  ] [ result   ] testcase (agent.device_tools)`。脚本除了数 PASSED，还要数 `[  ASSERT  ]` 行。这个内核的 utest 在每个单元组开始时清零失败计数（`rt-thread/components/utilities/utest/utest.c` 里的 `utest_unit_run()`），所以某个单元组里的断言失败可能被后面的单元组覆盖，最终照样打印 PASSED。脚本要求断言行数为 0。

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
| `utest` | 包内用例的 `name`、`passed_runs`、`failed_runs`、`assert_failures` |
| `network_enabled` | 固定为 `false`，QEMU 用了 `-net none` |
| `disk_snapshot` | 固定为 `true`，SD 卡挂了 `snapshot=on` |
| `summary_transport` | 固定为 `deterministic SSE fixtures at webclient post/read boundary` |

脚本退出前会自己判一遍，判据分三档：包内用例两轮都 PASSED、0 FAILED、0 断言行；`agent_e2e_test` 两轮都有最终回答，每轮 `checks` 不少于 5；技能、上下文、设备三组每轮 `checks` 不少于 20。四组结果和三个堆快照数组的长度都必须是 2，缺一条就判定失败。所有堆快照的 `delta` 必须为 0。任何一条不满足就抛异常，进程以非 0 退出。

---

## 4. 这套回归不能证明什么

回归覆盖了框架逻辑，但有三条硬边界。

- **模型是假的。** `mock_chat.c` 把模型换成固定 SSE 夹具。请求怎么写、参数怎么选、总结写得好不好，真实模型的行为没有被验证。端到端测试里的工具调用是夹具写死的，不是模型自己决定的。
- **外设是假的。** `utst0`、`utblk0`、`utrtc0` 以及端到端用的虚拟设备都是内存里的桩。真实驱动的时序、中断、DMA、掉电行为没有被覆盖。块设备不会真的擦写介质。
- **环境是隔离的。** 网络是关的，SD 卡是快照。MQTT、TTS、文件上传这类需要外网的路径在回归里跑不到。
- **配置是这份 BSP 的。** 固件里只有通用层和 GPIO、I2C、SPI 四个模块。PWM、ADC、DAC、传感器、LED 五个模块只做了编译检查，运行时行为要等到有对应驱动的板子上再看。模块表那条检查遍历的是当前编译进来的模块，不会因为某个模块缺席而报错。

另外，回归固件里的包内用例是 `SConstruct` 显式加进去的，不依赖 BSP 的 `RT_USING_UTEST` 配置。

---

## 5. 真机验证

板子上跑同一套检查，用 Debug 通道驱动对话。

### 5.1 通道选择

当前工程在 `rtconfig.h` 里定义了 `PKG_AGENT_DEBUG_CHANNEL`，包内 `src/channels/AgentDebugChannel.c` 提供实现。LVGL 通道暂时没开，它的实现由应用工程提供。

### 5.2 手动跑包内用例

打开 `RT_USING_UTEST` 后重新编译烧录，然后在串口执行：

```
msh /> utest_run -thread agent.device_tools
```

### 5.3 用对话验证设备工具

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

### 5.4 查日志

串口日志能看出工具调用的完整过程：每次调用的工具名、参数和返回文本。`[mem] cleanup done` 的 `used` 用来判断有没有泄漏，反复执行「进入对话 → 清理 → 再进入」看这个数是否持平。
