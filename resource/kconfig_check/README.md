# Agent Kconfig 校验（离线，不参与编译）

`SConscript` 只遍历 `src/`，所以本目录既不会被编译，也不会被 `menuconfig` 加载；
这里放的是 **Agent 包 Kconfig 的修改版**以及用来验证它的脚本。

| 文件 | 说明 |
|---|---|
| `Agent.Kconfig` | **交付用的修改版**（基于 `Agent.Kconfig.current`，改动见下） |
| `Agent.Kconfig.current` | 修改前的原文（从 `D:\rtt\env-windows\packages\packages\ai\Agent\Kconfig` 复制，便于 `git diff --no-index` 对比） |
| `stubs/Kconfig.base` | 依赖桩：klibc / mbedtls / webclient / pahomqtt(含 PIPE-UDP choice) / webnet / wavplayer 等外部符号（**不含** `RT_VER_NUM`） |
| `stubs/Kconfig.deps` | `source` 上面的桩 + 定义 `RT_VER_NUM`（真实内核里它是隐藏 hex 符号，这里给了 prompt 以便脚本模拟不同内核） |
| `kparse.py` | 解析检查：语法错误 + kconfiglib 警告（两个文件都能跑） |
| `scenarios.py` | 14 组内核/klibc 组合，断言 `PKG_USING_RT_VSNPRINTF_FULL` 的最终取值 |
| `inventory.py` | 符号清单：现有 `.config` 的每个 `PKG_AGENT*` 是否仍存在 + 代码引用是否齐全 |
| `audio_check.py` | 验证音频驱动 select 修复（旧文件里该选项实际无法生效） |
| `Kconfig.test` | 脚本生成的临时入口（source 桩 + 被测文件） |

运行（用 env 自带的 python + kconfiglib，无需编译）：

```powershell
$py = 'D:\rtt\env-windows\.venv\Scripts\python.exe'
& $py .\kparse.py Agent.Kconfig         # 也接受 Agent.Kconfig.current
& $py .\scenarios.py Agent.Kconfig
& $py .\inventory.py
& $py .\audio_check.py
```

## 改动一览（对比 `Agent.Kconfig.current`）

1. **`select PKG_USING_RT_VSNPRINTF_FULL` 条件化**
   原来两条 `select ... if`（重复 select 同一符号、且只看 `RT_VERSION_MAJOR < 5`）改为
   `select PKG_USING_RT_VSNPRINTF_FULL if PKG_AGENT_VSNPRINTF_NEED_PKG`，判定改由内核
   符号推导：
   - `PKG_AGENT_KLIBC_HAS_VSNPRINTF` = `RT_VER_NUM >= 0x40100 && !RT_KLIBC_USING_LIBC_VSNPRINTF`
   - `PKG_AGENT_KLIBC_FULL_VSNPRINTF` = 上面成立 + `RT_KLIBC_USING_VSNPRINTF_STANDARD` + DECIMAL specifiers
   - `PKG_AGENT_VSNPRINTF_PKG_USABLE` = `RT_VER_NUM < 0x50200`（该包自身 Kconfig 的限制）
   - `PKG_AGENT_VSNPRINTF_NEED_PKG` = `USE_PKG && PKG_USABLE` 或 `AUTO && !KLIBC_HAS && PKG_USABLE`
2. **删除 `select PAHOMQTT_PIPE_MODE`**：它是 paho 里 choice 的成员，select 无效
   （kconfiglib 原话 *"select/imply has no effect on choice symbols"*），改为注释说明。
3. **`PKG_AGENT_TOOL_MQTT_ENABLE` 的 help** 由 5 个工具补成 7 个（加 `mqtt_connect` /
   `mqtt_disconnect`），并说明 paho pipe 模式必须手动选。
4. **`PKG_AGENT_DRIVER_AUDIO`**：从 `if PKG_AGENT_MULTIMODAL_ENABLE` 里移出；`select`
   修正 `PKG_WP_USINGRECORD` → `PKG_WP_USING_RECORD`、`BSP_USING_AUDIO` → `RT_USING_AUDIO`。
5. **`PKG_AGENT_TOOL_MQTT_RX_THREAD_PRIO`** 的 `range 1 254` → `range 1 31`
   （`RT_THREAD_PRIORITY_MAX` 默认 32，越界会让 `rt_thread_create()` 断言）。
6. 新增 4 条 `comment` 提示行，直接显示自动判定结果（含「klibc 不支持 %f」的告警）。

## 校验结果

```
kparse.py    : parsed OK, warnings: 0
scenarios.py : ALL SCENARIOS PASSED (14/14)
inventory.py : 现有 .config 的 49 个 PKG_AGENT* 符号一个都没丢
audio_check  : 旧文件 PKG_AGENT_DRIVER_AUDIO=y 实际不生效(audio=n)，新文件生效(audio=y)
```

改动前 `scenarios.py` 的结果（5 处错误，都会被误选 `rt_vsnprintf_full` =
链接期 `multiple definition of 'rt_vsnprintf'`）：

| 场景 | 旧文件 | 新文件 | 期望 |
|---|---|---|---|
| 4.1.x + klibc std | **y** | n | n |
| 4.1.x + klibc tiny | **y** | n | n |
| 5.3.x + `RT_KLIBC_USING_LIBC_VSNPRINTF=y` | **y** | n | n（该包 5.2 起不可用） |
| 5.2.x + `RT_KLIBC_USING_LIBC_VSNPRINTF=y` | **y** | n | n |
| 5.3.x + 强制 `USE_PKG` | **y** | n | n |

## 关于 `RT_VER_NUM` 的一个坑（实测）

kconfiglib 对 int/hex 符号的表达式语义容易踩坑，已实测确认：

- `!RT_VER_NUM` 对**任何** int/hex 符号都恒为真 → 不能用来判断「未定义」；
- 与**未定义**符号的任何比较（`<` `>` `=`）恒为假；
- 因此 Kconfig 无法区分「符号不存在」和「比较为假」，所以本方案不做人工版本号兜底，
  极老内核（早于 4.0、无 `RT_VER_NUM`）需要手工在 rt_vsnprintf_full 自己的菜单里启用。
