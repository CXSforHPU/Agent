from building import *
import os

"""
Agent组件构建脚本
适用：RT-Thread SCons构建系统
功能：根据Kconfig依赖，选择性编译Agent核心、通道、工具、驱动代码
"""

# ===================== 路径常量定义（头部统一管理） =====================
# 当前脚本所在目录
CWD = GetCurrentDir()

# 源码根目录
SRC_ROOT = os.path.join(CWD, "src")
# 头文件根目录
INC_ROOT = os.path.join(CWD, "include")
# 配置文件目录
CFG_ROOT = os.path.join(CWD, "config")

# 子模块目录
SRC_CHANNELS = os.path.join(SRC_ROOT, "channels")
INC_CHANNELS = os.path.join(INC_ROOT, "channels")

SRC_TOOLS = os.path.join(SRC_ROOT, "tools")
INC_TOOLS = os.path.join(INC_ROOT, "tools")

SRC_TOOL_MQTT = os.path.join(SRC_TOOLS, "tool_mqtt")
INC_TOOL_MQTT = os.path.join(INC_TOOLS, "tool_mqtt")

SRC_TOOL_DEVICE = os.path.join(SRC_TOOLS, "tool_device")

SRC_UTILS = os.path.join(SRC_ROOT, "utils")

SRC_DRIVER_AUDIO = os.path.join(SRC_ROOT, "driver", "audio")
INC_DRIVER_AUDIO = os.path.join(INC_ROOT, "driver", "audio")

# =======================================================================

# 源码列表 & 头文件搜索路径列表
src = []
CPPPATH = []

# ---------------------- Core 核心源码 ----------------------
# 遍历src目录下所有.c文件
for filename in os.listdir(SRC_ROOT):
    file_fullpath = os.path.join(SRC_ROOT, filename)
    if filename.endswith(".c") and os.path.isfile(file_fullpath):
        src.append(file_fullpath)
# 添加核心头文件目录
CPPPATH.append(INC_ROOT)

if GetDepend(["PKG_USING_AGENT"]):
    src += list(SConscript("skills/SConscript"))

# ---------------------- Utils 工具模块 ----------------------
# 按模块拆成多个 c 文件，各自的 Kconfig 开关写在文件里，这里统一全收
for filename in os.listdir(SRC_UTILS):
    file_fullpath = os.path.join(SRC_UTILS, filename)
    if filename.endswith(".c") and os.path.isfile(file_fullpath):
        src.append(file_fullpath)

# ---------------------- Channel 通信通道模块 ----------------------
# CLI控制台通道
if GetDepend(["PKG_AGENT_CLI_CHANNEL"]):
    src.append(os.path.join(SRC_CHANNELS, "CLI.c"))
# Web网络通道
if GetDepend(["PKG_AGENT_WEBNET_CHANNEL"]):
    src.append(os.path.join(SRC_CHANNELS, "AgentWebChannel.c"))
# Debug调试通道
if GetDepend(["PKG_AGENT_DEBUG_CHANNEL"]):
    src.append(os.path.join(SRC_CHANNELS, "AgentDebugChannel.c"))
# 通道模块头文件
CPPPATH.append(INC_CHANNELS)

# ---------------------- Tools 工具集模块 ----------------------
# 基础工具：自动遍历tools目录下所有c文件
for filename in os.listdir(SRC_TOOLS):
    file_fullpath = os.path.join(SRC_TOOLS, filename)
    if filename.endswith(".c") and os.path.isfile(file_fullpath):
        src.append(file_fullpath)
CPPPATH.append(INC_TOOLS)

# MQTT工具（Kconfig开关控制，模块拆分为多个c文件：引擎/订阅生命周期/路由/过滤/业务/工具入口/MSH）
if GetDepend(["PKG_AGENT_TOOL_MQTT_ENABLE"]):
    for filename in os.listdir(SRC_TOOL_MQTT):
        file_fullpath = os.path.join(SRC_TOOL_MQTT, filename)
        if filename.endswith(".c") and os.path.isfile(file_fullpath):
            src.append(file_fullpath)
    CPPPATH.append(INC_TOOL_MQTT)

# 设备框架工具（总开关收整个目录，每类设备的 PKG_AGENT_TOOL_DEVICE_XXX 开关写在文件里，
# 关掉即整文件不参与编译，模块自注册宏也就不会执行）
if GetDepend(["PKG_AGENT_TOOL_DEVICE"]):
    for filename in os.listdir(SRC_TOOL_DEVICE):
        file_fullpath = os.path.join(SRC_TOOL_DEVICE, filename)
        if filename.endswith(".c") and os.path.isfile(file_fullpath):
            src.append(file_fullpath)
    CPPPATH.append(SRC_TOOL_DEVICE)

# ---------------------- Test utest 用例 ---------------------- # RT_USING_UTEST 打开时编译包内 utest 测试用例
SRC_TEST = os.path.join(CWD, "test")
if GetDepend(["RT_USING_UTEST"]) and os.path.isdir(SRC_TEST):
    for filename in os.listdir(SRC_TEST):
        file_fullpath = os.path.join(SRC_TEST, filename)
        if filename.endswith(".c") and os.path.isfile(file_fullpath):
            src.append(file_fullpath)

# ---------------------- Config 配置模块 ----------------------
src.append(os.path.join(CFG_ROOT, "AgentConfig.c"))
CPPPATH.append(CFG_ROOT)

# ---------------------- Driver 驱动模块 ----------------------
# Audio音频驱动
if GetDepend(["PKG_AGENT_DRIVER_AUDIO"]):
    src.append(os.path.join(SRC_DRIVER_AUDIO, "AgentAudio.c"))
    CPPPATH.append(INC_DRIVER_AUDIO)

# ---------------------- 构建组件分组 ----------------------
group = DefineGroup(
    name="Agent",
    src=src,
    depend=["PKG_USING_AGENT"],
    CPPPATH=CPPPATH
)

Return("group")