# Agent Kconfig 校验（离线，不参与编译）

`SConscript` 不收录本目录，所以这里的文件既不会被编译，也不会被 `menuconfig` 加载。
这里放的是校验脚本，被测对象是软件包自己的 `Kconfig`（包根目录）。

| 文件 | 说明 |
|---|---|
| `stubs/Kconfig.base` | 依赖桩：klibc / mbedtls / webclient / pahomqtt(含 PIPE-UDP choice) / webnet / wavplayer 等外部符号（**不含** `RT_VER_NUM`） |
| `stubs/Kconfig.deps` | `source` 上面的桩 + 定义 `RT_VER_NUM`（真实内核里它是隐藏 hex 符号，这里给了 prompt 以便脚本模拟不同内核） |
| `kparse.py` | 解析检查：语法错误 + kconfiglib 警告 |
| `scenarios.py` | 14 组内核/klibc 组合，断言 `PKG_USING_RT_VSNPRINTF_FULL` 的最终取值 |
| `inventory.py` | 符号清单：现有 `.config` 的每个 `PKG_AGENT*` 是否仍存在 + 代码引用是否齐全 |
| `audio_check.py` | 验证音频驱动的 select 是否真的生效 |
| `Kconfig.test` | 脚本生成的临时入口（source 桩 + 被测文件） |

`Kconfig.test`、`.config.audio`、`.config.scen` 是脚本生成的临时文件，已在 `.gitignore` 里。

## 运行

用 env 自带的 python + kconfiglib，无需编译：

```powershell
$py = '<env-windows>\.venv\Scripts\python.exe'   # 本机 RT-Thread Env 安装目录
& $py .\kparse.py                 # 解析包根目录的 Kconfig
& $py .\kparse.py --values        # 顺带打印 PKG_AGENT* 的取值
& $py .\scenarios.py              # 14 组内核组合的 vsnprintf 判定
& $py .\inventory.py              # .config 与 Kconfig 的符号差集
& $py .\audio_check.py            # 音频驱动开关
```

四个脚本都按自身位置定位目录，不写死本机路径。导入 `kconfiglib` 失败时会提示改用 env 自带的 python。

## 为什么校验 vsnprintf

`rt_vsnprintf` 由内核 klibc 定义。包再选一次 `pkgs/rt_vsnprintf_full`，链接期会报
`multiple definition of 'rt_vsnprintf'`。判定依据是「内核是否已经定义该符号」，
而不是只看内核版本号：

- `RT_KLIBC_USING_LIBC_VSNPRINTF=n`（默认）：klibc 编译 `rt_vsnprintf_std.c` 或
  `rt_vsnprintf_tiny.c`，两者都是强符号 `int rt_vsnprintf(...)`，内核已定义该符号。
- `RT_KLIBC_USING_LIBC_VSNPRINTF=y`：klibc 直接调 libc 的 `vsnprintf`，内核不定义该符号，
  包可以提供（仅 RT-Thread < 5.2，因为该包自身 Kconfig 限制 `RT_VER_NUM < 0x50200`）。

`scenarios.py` 覆盖 14 组组合，每组断言「是否选中 `PKG_USING_RT_VSNPRINTF_FULL`」。

| 场景 | 选中 | 期望 |
|---|---|---|
| 4.0.x：klibc 没有 rt_vsnprintf | y | y |
| 4.1.x + klibc std | n | n |
| 4.1.x + klibc tiny | n | n |
| 5.3.x + `RT_KLIBC_USING_LIBC_VSNPRINTF=y` | n | n（该包 5.2 起不可用） |
| 5.2.x + `RT_KLIBC_USING_LIBC_VSNPRINTF=y` | n | n |
| 5.3.x + 强制 `USE_PKG` | n | n |
| 5.1.x + `RT_KLIBC_USING_LIBC_VSNPRINTF=y` | y | y |

## 关于 `RT_VER_NUM` 的一个坑（实测）

- kconfiglib 对 int/hex 符号的表达式语义容易踩坑，已实测确认：
- `!RT_VER_NUM` 对**任何** int/hex 符号都恒为真，不能用来判断「未定义」；
- 与**未定义**符号的任何比较（`<` `>` `=`）恒为假；
- 因此 Kconfig 无法区分「符号不存在」和「比较为假」，所以本方案不做人工版本号兜底，
  极老内核（早于 4.0、无 `RT_VER_NUM`）需要手工在 rt_vsnprintf_full 自己的菜单里启用。
