# MCP 服务端部署

设备端的 MCP 客户端只支持 HTTP 传输（Streamable HTTP），不能在设备上拉起本地进程。
这个目录提供主机侧的服务端：把 Windows 上的 MCP 服务器统一挂到一个 HTTP 端点上，
设备端只配一个地址就能用。

只用 Python 3 标准库，不需要 pip install。

## 1. 文件

| 文件 | 说明 |
|---|---|
| `mcp_gateway.py` | 组装根。解析配置、建对象、装工具、起服务 |
| `config.py` | 配置层。命令行和环境变量只在这里解析一次，往下的层只读配置对象 |
| `gateway/transport.py` | HTTP 传输。把 `/mcp` 上的 HTTP 请求翻成 JSON-RPC 调用，再把回包写回去 |
| `gateway/service.py` | 用例层。按名字找到工具、跑它、把结果装进回包 |
| `gateway/registry.py` | 工具表。注册工具、编对外的名字、按名字找工具 |
| `gateway/shaping.py` | 回包裁剪。base64 媒体落盘，文字按预算截断 |
| `gateway/runtime.py` | 后台事件循环。工具的 `execute` 都在这个循环里跑 |
| `gateway/protocol.py` | 协议常量和 JSON-RPC 回包的样子 |
| `tools/base.py` | 工具基类 `Tool`。继承它就能写一个自己的工具，`to_content` 三个参数包返回结果 |
| `tools/context.py` | 工具调用的上下文。文件服务器和配置从这里拿 |
| `tools/loader.py` | 扫描用户工具目录，把里面继承 `Tool` 的类实例化 |
| `tools/media.py` | 文件服务器客户端和媒体的落盘，图片和音频存上去换回下载地址 |
| `tools/file/` | 内置文件工具，一个工具一个模块一个类，工具名和参数与 Windows 文件资源管理器 MCP 连接器一致 |
| `tools/user_tools/` | 工具模块目录，网关启动时自动发现。用户自己的工具放这里，联网搜索也在这里，见第 6 节 |
| `tools/remote.py` | 外部 MCP 服务器报上来的工具，用 `RemoteTool` 包一层 |
| `tools/odr.py` | Windows 11 接入：用 `odr.exe list` 找到 ODR 注册的服务器，按清单用 stdio 拉起 |
| `tools/command.py` | 命令行接入：把一条启动命令当成 MCP 服务器，用 stdio 拉起并转发它的工具 |
| `tools/stdio.py` | MCP 的 stdio 传输客户端，换行分隔的 JSON-RPC 报文 |
| `start_mcp_gateway.bat` | Windows 启动脚本，顺带装好并接入 Windows-MCP |
| `setup_windows_mcp.bat` | 找好或装好 Windows-MCP，把启动命令留在 `MCP_CMD` 里，deploy 脚本也用它 |
| `start_mcp_gateway.sh` | Linux、macOS 启动脚本 |
| `test_gateway.py` | 主机侧自测脚本，按设备端的流程走一遍握手和工具调用 |
| `../` | 文件服务器，工具回包里的图片存在这里，见第 5.6 节 |

### 1.1 分层

代码按层拆开，依赖只向里指：外层可以叫里层，里层不反过来叫外层。

```
传输层    gateway/transport.py     HTTP 收发
用例层    gateway/service.py       调度、找工具、装回包
工具表    gateway/registry.py      注册、命名、查找
工具层    tools/                   工具的实现
```

每层的职责：

| 层 | 做什么 | 不做什么 |
|---|---|---|
| 传输层 | 收 HTTP 请求，发 JSON-RPC，回写响应 | 不认识工具 |
| 用例层 | 弄清请求要干什么，跑工具，装回包 | 不认识 HTTP，不读配置文件 |
| 工具表 | 管工具的注册、对外名字和查找 | 不认识 HTTP |
| 工具层 | 真正干活，把结果包成 content 数组 | 不 import 网关 |

工具层在 `tools/` 目录里，它不 import `gateway/`。工具要配置就实现
`configure(context)`，网关注册的时候调一次，`context` 里有文件服务器和配置
对象。工具要当前的上下文就用 `current_context()`。

配置文件由 `config.py` 一处解析：命令行优先，其次环境变量，最后默认值。工具
自己不读 `os.environ`。

### 1.2 工具只有一种

工具只有 `Tool` 一个类，而且是基类：所有的工具都是继承它的类。内置文件工具、
联网搜索、外部 MCP 服务器、用户自己的工具，最后都是一串 `Tool`，网关不分来源，
按名字路由。

```python
from tools.base import Tool, to_content


class EchoTool(Tool):
    @property
    def name(self):
        return "echo"

    @property
    def description(self):
        return "Echo the given text back. Read-only."

    @property
    def parameters(self):
        return {"type": "object", "properties": {}}

    async def execute(self, **kwargs):
        return await to_content(text="hello")
```

`execute` 是协程，写成 `async def`。工具里有阻塞的活儿，用
`await asyncio.to_thread(...)` 丢到线程里跑。

网关起一个事件循环，所有工具的 `execute` 都在这个循环里跑。工具之间互不阻塞：
一个工具在等网络的时候，别的工具照样能调。

### 1.3 工具从哪来

| 来源 | 工具代码 | 对外名字 |
|---|---|---|
| 内置文件工具 | `tools/file/`，一个模块一个类 | 工具原名，例如 `read_text_file` |
| 联网搜索 | `tools/user_tools/web_search_tool.py` | `web_search` |
| 用户工具 | `tools/user_tools/`，网关启动时扫描目录 | `user__` 加工具名 |
| 命令行服务器 | `tools/command.build_tools()` | 服务器名加 `__` 加工具名 |
| ODR 服务器 | `tools/odr.build_tools()` | 服务器名加 `__` 加工具名 |

## 2. 启动

推荐用上层的 deploy 脚本，它默认把文件服务器和这个网关一起起起来：

```bat
cd packages\Agent-latest\resource\file_server
deploy.bat
```

网关默认听 8765，板端填 `PKG_AGENT_MCP_SERVER_URL=http://PC-IP:8765/mcp`。只想要文件服务器时加 `--no-mcp`，换端口用 `--mcp-port 8766`。用 `--start` 点名组件时，`mcp` 不在名单里网关就不起。

单独起网关用本目录的脚本。

Windows：

```bat
cd packages\Agent-latest\resource\file_server\mcp
start_mcp_gateway.bat 8765 C:\Users\me
```

第一个参数是端口，默认 8765。第二个参数是文件工具可访问的根目录，默认当前用户主目录。
后面的参数原样传给 `mcp_gateway.py`。

脚本会顺带接入 Windows-MCP。机器上没有 uv 就先装 uv，没有 windows-mcp 包就再装包，
细节见第 5.1 节。

Linux、macOS：

```sh
cd packages/Agent-latest/resource/file_server/mcp
./start_mcp_gateway.sh 8765 "$HOME"
```

也可以直接跑 Python：

```sh
python3 mcp_gateway.py --host 0.0.0.0 --port 8765 --root "$HOME"
```

常用参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址，设备要连别的机器时不能只听 127.0.0.1 |
| `--port` | `8765` | 监听端口 |
| `--root` | 当前用户主目录 | 文件工具可访问的根目录，目录外的路径一律拒绝 |
| `--no-odr` | 关闭 | 不接入 ODR，只用内置文件工具 |
| `--windows-mcp` | 关闭 | 接入 Windows-MCP，设备就能控制本机桌面，见第 5 节 |
| `--no-windows-mcp` | 关闭 | 不接入 Windows-MCP，脚本默认是接入的 |
| `--windows-mcp-cmd` | `uvx windows-mcp serve` | Windows-MCP 的启动命令，整条命令原样传下去 |
| `--server-timeout` | `60` | 外部 MCP 服务器启动与握手的超时秒数 |
| `--call-timeout` | `60` | 一个工具最多跑多久秒 |
| `--max-text-bytes` | `24000` | 单次回包里文本的字节上限，要小于板端的 `PKG_AGENT_MCP_RESP_BUFSZ` |
| `--file-server` | `http://127.0.0.1:8000` | 图片存到哪个文件服务器，见第 5.6 节 |
| `--file-server-url` | 跟 `--file-server` 一样 | 回包里的下载地址用什么基地址 |
| `--no-file-server` | 关闭 | 不接文件服务器，图片直接换成占位说明 |
| `--user-tools` | 无 | 额外的用户工具目录，可以给多次。内置的 `tools/user_tools` 始终加载 |
| `--no-user-tools` | 关闭 | 不加载用户自己的工具，内置目录也不加载 |
| `--web-search-url` | `https://searx.be` | 联网搜索的地址，见第 7 节，也认环境变量 `MCP_WEB_SEARCH_URL` |
| `--web-search-key` | 空 | 联网搜索的密钥，Bing 和 Brave 用得上，也认环境变量 `MCP_WEB_SEARCH_KEY` |
| `--web-search-mode` | `auto` | 对接方式：`auto`、`searxng`、`bing`、`brave`、`post` |
| `--web-search-timeout` | `15` | 联网搜索的超时秒数，也认环境变量 `MCP_WEB_SEARCH_TIMEOUT` |
| `--web-search-max-results` | `5` | 一次最多返回几条结果，最多 10 条，也认环境变量 `MCP_WEB_SEARCH_MAX_RESULTS` |
| `--no-web-search` | 关闭 | 不接入联网搜索工具，也认环境变量 `MCP_WEB_SEARCH_ENABLED` |

## 3. 设备端配置

设备端的地址由 Kconfig 选项 `PKG_AGENT_MCP_SERVER_URL` 决定，默认值是
`http://10.0.2.2:8765/mcp`。把它改成网关所在机器的 IP：

```
CONFIG_PKG_AGENT_TOOL_MCP_ENABLE=y
CONFIG_PKG_AGENT_MCP_SERVER_URL="http://192.168.1.100:8765/mcp"
```

改完重新编译固件。网关的 IP 用 `ipconfig` 查，设备要和它在同一网段。

这个选项在固件的 `External Server` 菜单里。菜单里还有一个来源选择：手动填死，或者启动时向管理控制台问一次。选自动模式时，上面填的地址是控制台连不上时的回退值，端口改了不用重新烧固件。控制台那边在运维页的「板端地址」填板子够得着的主机名。

## 4. Windows 上的 MCP 服务器

Windows 11 的 ODR（设备代理注册表）里注册了 MCP 服务器，`odr.exe list` 返回 JSON 数组，
每项的 `manifest.server.mcp_config` 里有 `command` 和 `args`。这些服务器用 stdio 传输，
网关按清单把进程拉起来，再把它们的工具转发给设备。工具名带服务器前缀，例如
`<服务器名>__search_files`。

命令行给的服务器用同一条路径。Windows-MCP 就是这样接的，见第 5 节。

Windows 10 没有 ODR，网关只提供内置文件工具。内置工具的名称和参数与 Windows 文件资源
管理器 MCP 连接器一致：`get_file_details`、`get_directories`、`read_text_file`、`read_file`、
`search_files`、`create_directory`、`create_text_file`、`edit_text_file`、`move_file`、
`zip_folder`、`unzip_folder`。

## 5. 接入 Windows-MCP

[Windows-MCP](https://github.com/CursorTouch/Windows-MCP) 是一个开源的 Windows 桌面控制
MCP 服务器。它能开应用、读屏幕、点鼠标、打字、跑 PowerShell、管进程和注册表。
0.8.5 版是 20 个工具，0.8.8 版多一个 `ControlStatus`，一共 21 个。

设备端不能直连它。Windows-MCP 的 HTTP 传输按 FastMCP 的规矩回包，
`content-type: text/event-stream`，分块传输。webclient v2.2.0 认不出
`Transfer-Encoding: chunked`，会把响应体一直读到超时。设备端还得在 `Accept` 里同时
声明 `application/json` 和 `text/event-stream`，只写一个服务端就回 406。

所以还是要过网关。网关用 stdio 把 Windows-MCP 拉起来，再把工具换成普通 JSON 回给设备。

### 5.1 准备 uv 和 Windows-MCP

Windows-MCP 用 uv 分发。启动脚本自己会把这三步做完：

1. 找 `uv`。找不到就从 <https://astral.sh/uv> 装，装到 `%USERPROFILE%\.local\bin`。
2. 找 `windows-mcp` 这个可执行文件。没有就用 `uv tool install windows-mcp` 装。
3. 装不上就退回 `uvx windows-mcp serve`，它第一次跑会下载包。

用到的命令会打印在启动日志里：

```
[mcp] Windows-MCP command: C:\Users\me\.local\bin\windows-mcp.exe
```

这一步要联网。第一次装 uv 加装包要一两分钟，装完之后的启动都走缓存。

### 5.2 启动

```bat
cd packages\Agent-latest\resource\file_server\mcp
start_mcp_gateway.bat 8765 C:\Users\me
```

脚本已经把 `--windows-mcp` 加上去了，不用再写。要换启动命令，把 `--windows-mcp-cmd`
放在最后，它后面的内容都算启动命令：

```bat
start_mcp_gateway.bat 8765 C:\Users\me --windows-mcp-cmd uvx windows-mcp serve
```

不要桌面控制工具时加 `--no-windows-mcp`：

```bat
start_mcp_gateway.bat 8765 C:\Users\me --no-windows-mcp
```

启动后终端会打印一行 `[mcp] 已接入服务器 win，工具 20 个`。这行没出来，说明
Windows-MCP 没起来，看它后面的提示。

### 5.3 工具名对应

网关把远端工具名压成小写，非字母数字的换成下划线，再加 `win__` 前缀。
设备端再补一层 `mcp_`，所以设备上看到的名字是 `mcp_win__click` 这样。

| Windows-MCP 的工具 | 网关暴露的名字 | 设备端看到的名字 |
|---|---|---|
| `App` | `win__app` | `mcp_win__app` |
| `Clipboard` | `win__clipboard` | `mcp_win__clipboard` |
| `Click` | `win__click` | `mcp_win__click` |
| `ControlStatus` | `win__controlstatus` | `mcp_win__controlstatus` |
| `DisplayInventory` | `win__displayinventory` | `mcp_win__displayinventory` |
| `FileSystem` | `win__filesystem` | `mcp_win__filesystem` |
| `Move` | `win__move` | `mcp_win__move` |
| `MultiEdit` | `win__multiedit` | `mcp_win__multiedit` |
| `MultiSelect` | `win__multiselect` | `mcp_win__multiselect` |
| `Notification` | `win__notification` | `mcp_win__notification` |
| `PowerShell` | `win__powershell` | `mcp_win__powershell` |
| `Process` | `win__process` | `mcp_win__process` |
| `Registry` | `win__registry` | `mcp_win__registry` |
| `Scrape` | `win__scrape` | `mcp_win__scrape` |
| `Screenshot` | `win__screenshot` | `mcp_win__screenshot` |
| `Scroll` | `win__scroll` | `mcp_win__scroll` |
| `Shortcut` | `win__shortcut` | `mcp_win__shortcut` |
| `Snapshot` | `win__snapshot` | `mcp_win__snapshot` |
| `Type` | `win__type` | `mcp_win__type` |
| `Wait` | `win__wait` | `mcp_win__wait` |
| `WaitFor` | `win__waitfor` | `mcp_win__waitfor` |

`ControlStatus` 是 0.8.8 才有的，0.8.5 没有这一条。

### 5.4 权限

Windows-MCP 用 stdio 跑在本机，不走网络，也不需要额外的密钥。

网关的 HTTP 端点本身没有认证。拿到这个地址的人就能控制这台电脑。把端口只开在可信网段，
或者在系统防火墙里限制来源 IP。设备端要和 PC 在同一网段。

### 5.5 设备端的容量

Windows-MCP 的工具表 20 项（0.8.8 是 21 项），加上内置文件工具、用户工具和联网搜索
一共 35 到 36 项，超过了 `PKG_AGENT_MCP_MAX_TOOLS` 的旧默认值 16。包里的 Kconfig
已把默认值改成 40。

`tools/list` 的回包有 27 KB，超过了 `PKG_AGENT_MCP_RESP_BUFSZ` 的旧默认值 8192。
包里的 Kconfig 已把默认值改成 32768。Windows-MCP 的工具描述也长，最长的一条有 1125 个字符，
设备端的描述缓冲区从 192 字节放大到了 1280 字节。

大回包也靠网关裁剪。`win__screenshot` 回的是一张整屏 PNG，base64 有 780 KB；
`win__snapshot` 的桌面结构有 29 KB。板端 32 KB 的缓冲装不下这两种。网关在转发前
把图片存到文件服务器，回包里只留一句带下载地址的说明；超长的文本截断到
`--max-text-bytes`（默认 24000 字节），并在结尾标出省了多少字符。截图回包因此从
780 KB 变成 961 字节，桌面结构从 29 KB 变成 24 KB。

只经过网关的链路才有这个裁剪。板子直连别的 MCP 服务端时，大回包会撞上
`PKG_AGENT_MCP_RESP_BUFSZ` 的上限，客户端按失败处理。

### 5.6 图片和音频存到文件服务器

媒体的落盘由网关做，板子不碰这些文件。两条路都归到 `tools/media.py`。

外部 MCP 服务器（Windows-MCP 等）回的 base64 媒体，网关解成二进制，multipart
上传到文件服务器的 `/upload`，回包里换成一句说明：

```
[image saved: http://127.0.0.1:8000/download/<file_id> (image/png, 516817 bytes)]
[audio saved: http://127.0.0.1:8000/download/<file_id> (audio/wav, 1644 bytes)]
```

用户工具用 `to_content(image=...)`、`to_content(audio=...)`，函数把文件存上去，
回包里给的是一条带地址的条目：

```json
{"type": "image", "url": "http://127.0.0.1:8000/download/<file_id>"}
{"type": "audio", "url": "http://127.0.0.1:8000/download/<file_id>"}
```

板端的 `result_to_text` 把这类条目渲染成一行 `[image: <地址>]` 或 `[audio: <地址>]`。
模型和人都拿得到地址，浏览器打开就是那张图或那段音频。板端一次只收 32768 字节，
整段 base64 上不去。

MIME 的子类型和扩展名的对应见 `tools/media.py` 里的 `MEDIA_SUFFIX`，覆盖 wav、
mp3、aac、ogg、flac、amr、m4a 这些常见格式。文件服务器先按魔数嗅探真实类型，
扩展名只是兜底，所以 `.wav` 存进去下载回来还是 `audio/wav`。

文件服务器是包里的 `resource/file_server`，用 FastAPI 写的，接口见
[resource/file_server/framework.md](../resource/file_server/framework.md)。起法：

```sh
cd resource/file_server
bash deploy.sh --start-only      # Windows 用 deploy.bat --start-only
```

网关默认就接 `http://127.0.0.1:8000`。两个参数能改：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--file-server` | `http://127.0.0.1:8000` | 媒体上传到哪个文件服务器 |
| `--file-server-url` | 跟 `--file-server` 一样 | 回包里的下载地址用什么基地址 |
| `--no-file-server` | 关 | 不接文件服务器，图片和音频直接换成占位说明 |

上传的地址和下载地址可以分开。文件服务器在内网、模型在云上时，模型只能抓公网
地址，下载地址要填公网那个：

```sh
python mcp_gateway.py --windows-mcp \
    --file-server http://127.0.0.1:8000 \
    --file-server-url https://你的域名
```

文件服务器没起或者上传失败时，网关退回占位说明，调用照样成功，只是文件没存下来。

## 6. 用户自己的工具

不想改网关代码，就想在网关上加自己的工具，写一个继承 `Tool` 的类，
放进 `tools/user_tools/` 目录就行。网关启动时自动发现并注册。

这个目录里也有网关自带的联网搜索（`web_search_tool.py`），同样是继承基类的
一个工具类。

### 6.1 写一个工具

`tools/base.py` 里的 `Tool` 是基类，子类要实现四个成员：

| 成员 | 说明 |
|---|---|
| `name` | 工具名。字母、数字、下划线 |
| `description` | 工具说明。这段文字会发给模型，写清楚这个工具做什么 |
| `parameters` | 参数表，JSON Schema，和 `tools/list` 里的 `inputSchema` 一样 |
| `execute` | 执行工具。写成 `async def`，参数按 `parameters` 里的名字传进来，返回一段文字或一个 content 数组 |

子类要能用无参构造函数，构造函数不带参数。

工具要读配置就实现 `configure(context)`。网关注册工具的时候调一次，`context`
里有文件服务器和配置对象：

```python
    def configure(self, context):
        self.url = context.setting("web_search.url", "")
```

工具自己说现在不能用，就把 `enabled` 置成 `False`，网关跳过它，不注册。

```python
    enabled = False
```

```python
from tools.base import Tool, to_content


class EchoTool(Tool):
    @property
    def name(self):
        return "echo"

    @property
    def description(self):
        return "Echo the given text back. Read-only."

    @property
    def parameters(self):
        return {
            "type": "object",
            "properties": {"text": {"type": "string", "title": "text"}},
            "required": ["text"],
        }

    async def execute(self, text="", **kwargs):
        return await to_content(text=text)
```

`execute` 写成 `async def`，网关在事件循环里等它。工具里有阻塞的活儿，
用 `await asyncio.to_thread(...)` 丢到线程里跑，别直接卡住事件循环。
一个工具在跑的时候，别的工具照样能调。

`tools/user_tools/` 里有三个现成的例子，一个模块一个工具：`echo_tool.py`、
`now_tool.py`、`audio_tool.py`。同一个目录里的 `web_search_tool.py` 是网关自带的
联网搜索。照着抄一份改成自己的工具就行。

### 6.2 返回图片和音频

`to_content` 有三个参数，文字、图片、音频可以一起给：

```python
from tools.base import Tool, to_content

    async def execute(self, **kwargs):
        return await to_content(text="拍好了", image="D:/tmp/shot.png")
```

| 参数 | 能给什么 |
|---|---|
| `text` | 一段文字 |
| `image` | 本地文件路径、原始字节，或者 http 地址 |
| `audio` | 本地文件路径、原始字节，或者 http 地址 |

图片和音频会存到文件服务器，回包里给的是一条带地址的条目：

```json
{"type": "image", "url": "http://127.0.0.1:8000/download/<file_id>"}
```

http 地址直接用，不上传。原始字节按类型定 MIME，图片是 `image/png`，
音频是 `audio/wav`；本地文件按扩展名定，MIME 的对应表在 `tools/media.py` 里。

板端的 32 KB 接收缓冲装不下整段 base64，所以走文件服务器这条路。细节见第 5.6 节。

没接文件服务器时 `to_content(image=...)` 会报错，工具按失败返回，失败原因
写进 content。

### 6.3 目录约定

- 工具放在 `tools/user_tools/` 目录下，文件名以 `_` 开头的不会被加载。
- 一个模块一个工具类，模块名和工具名对上更清楚。
- 导入的基类不算，`from tools.base import Tool` 引进来的 `Tool` 本身不会被注册。
- 一个文件出错不影响别的文件，网关的日志里会打印原因。
- 类里写了 `namespace` 的工具用自己的名字，没写的按用户工具加 `user__`。

### 6.4 工具名

对外暴露的名字带 `user__` 前缀，网关不再加别的前缀。板端收到这个名字后
按 MCP 的规则加 `mcp_` 前缀，模型看到的是 `mcp_user__echo`。

工具名有两个前缀，分开用：

| 前缀 | 什么时候用 | 用户工具的值 |
|---|---|---|
| namespace | 一直带在名字前面，用来区分来源 | `user__` |
| prefix | 只在和别的工具撞名时用上 | `user__` |

类里写了 `namespace` 的工具不受这条规则约束。联网搜索就是这么做的，
它写 `namespace = ""`，对外就叫 `web_search`，模型看到的是 `mcp_web_search`。

撞名的那个工具改用 prefix 加原名，还是重就再加序号。在 `tools/list`
的结果里能看到最终名字。

### 6.5 换目录

内置的 `tools/user_tools/` 始终加载。要读别的地方用 `--user-tools`，
这个参数可以给多次：

```sh
python mcp_gateway.py --user-tools D:\my-tools
```

不想要用户工具就加 `--no-user-tools`，内置目录也不加载，联网搜索一起没有。

### 6.6 自测

`test_gateway.py` 的离线部分会检查这条链路：目录里的工具有没有被发现、
工具描述的三个字段齐不齐、调用能不能通、工具内部抛错会不会按失败返回、
同名的工具是不是只留一个、一个模块是不是只写了一个工具类。

媒体另有一组检查：`to_content` 的三种参数、图片和音频存上去之后回包里是不是
带地址的条目、扩展名按 MIME 定、http 地址不重复上传、没接文件服务器时报错、
100 KB 的音频回包不超板端上限。

配置也有一组检查：命令行的默认值、环境变量能改搜索服务地址、工具从上下文里
读到配置、说不能用就不注册。

## 7. 联网搜索

网关自带一个联网搜索工具，模型可以直接问它。工具名是 `web_search`，
板端按 MCP 的规则加前缀，模型看到的是 `mcp_web_search`。

工具类在 `tools/user_tools/web_search_tool.py`，和用户自己的工具一样由目录
扫描发现。配置由 `config.py` 解析好，注册的时候交给工具。

搜索服务是一个 HTTP 接口。网关按配置去问它，把结果整理成 JSON 给模型：

```json
{
  "query": "RT-Thread",
  "count": 3,
  "results": [
    {"title": "标题", "url": "https://example.com", "snippet": "摘要"}
  ]
}
```

调用过程：

![联网搜索的调用过程](../../../../docs/diagrams/mcp-web-search.png)

### 7.1 对接哪些搜索服务

| 对接方式 | 请求怎么发 | 密钥放哪 |
|---|---|---|
| `searxng` | GET，`q` 加 `format=json` | 不用密钥 |
| `bing` | GET，`q` | 请求头 `Ocp-Apim-Subscription-Key` |
| `brave` | GET，`q` | 请求头 `X-Subscription-Token` |
| `post` | POST，请求体是 `{"query": ..., "count": ...}` | 请求体里的 `api_key`，同时发 `Authorization` 头 |

`auto` 从地址里认。地址带 `searx` 按 SearXNG，带 `bing` 按 Bing，
带 `brave` 按 Brave，认不出来就按 `post` 处理。

各家搜索服务的回包形状不一样。Bing 放在 `webPages.value` 里，Tavily 和
SearXNG 放在 `results` 里。网关按第一个带链接的列表认结果，这三种都能解。

### 7.2 配一个搜索服务

自己起一个 SearXNG 最省事，不用密钥：

```sh
python mcp_gateway.py --web-search-url http://192.168.1.100:8080/search
```

用公开实例就用默认值 `https://searx.be`。国内网络常常连不上，连不上就自己起一个，
或者换一个能连的地址。

要密钥就写环境变量，不要写进命令行：

```sh
export MCP_WEB_SEARCH_URL=https://api.bing.microsoft.com/v7.0/search
export MCP_WEB_SEARCH_KEY=你的密钥
python mcp_gateway.py
```

网关只打印地址和对接方式，不打印密钥。

### 7.3 不想要这个工具

加 `--no-web-search`。网关不注册它，工具表里就没有这个工具。

### 7.4 环境变量

命令行和环境变量由 `config.py` 一处解析。命令行优先，其次环境变量，最后默认值。

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MCP_WEB_SEARCH_URL` | `https://searx.be` | 搜索服务地址 |
| `MCP_WEB_SEARCH_KEY` | 空 | 密钥，Bing 和 Brave 用得上 |
| `MCP_WEB_SEARCH_MODE` | `auto` | 对接方式 |
| `MCP_WEB_SEARCH_TIMEOUT` | `15` | 超时秒数 |
| `MCP_WEB_SEARCH_MAX_RESULTS` | `5` | 一次最多返回几条 |
| `MCP_WEB_SEARCH_ENABLED` | `1` | 置 `0` 就不注册这个工具 |

### 7.5 自测

`test_gateway.py` 的离线部分检查这条链路：工具表里有一个 `web_search`、
参数表里有必填的 `query`、没有 query 时按失败返回、搜索服务不可达时按失败返回、
没配地址时按失败返回、Bing 和 Tavily 和 SearXNG 三种回包都能解出结果、
对接方式能从地址里认出来。

配置另有一组检查：命令行的默认值、`--no-web-search` 关掉注册、环境变量能改
搜索服务地址、工具从上下文里读到配置、说不能用就不注册。

联机部分会调一遍 `web_search`，看回包里有没有文字。

## 8. 自测

网关起来之后，先在 PC 上跑一遍：

```sh
python test_gateway.py --url http://127.0.0.1:8765/mcp
```

脚本按设备端的顺序发 `initialize`、`notifications/initialized`、`tools/list`、`tools/call`，
并检查会话 ID 有没有按预期回传。网关开了用户工具时，还会调一遍 `user__echo`
和一遍 `user__audio`，看用户工具和音频回包能不能从 HTTP 这一侧调通。
网关开了联网搜索时，还会调一遍 `web_search`。
全部通过会打印「自测通过」。

设备端还有一条离线的回归用例，在 QEMU 里跑：

```
mcp_qemu_test
```

### 8.1 QEMU 真链路

离线的用例全程用替身，不出网。想验证板子真能连到 PC 上的网关，
用仓库里的联网脚本：

```sh
python build/skill_qemu/run_live.py
```

脚本做五件事：

1. 起文件服务器，端口 8000，工具回包里的图片存在这里。
2. 起主机侧网关，带 `--windows-mcp`，端口 8765。
3. 起本地搜索服务的替身，端口 8799，网关的 `web_search` 调它。
4. 起 QEMU，带 SLIRP 网卡。宿主在 `10.0.2.2`，板子 DHCP 拿到 `10.0.2.10`。
5. 在板子上跑 `mcp_live_qemu_test`，把结果写进 `build/skill_qemu/result_live.json`。

板子的 `PKG_AGENT_MCP_SERVER_URL` 要指向 `http://10.0.2.2:8765/mcp`。
用 SLIRP 时宿主地址固定是 `10.0.2.2`，不是 PC 的真实 IP。

用例里的第一条检查是「替身放行」。离线回归启动时，`mock_chat` 会拦下
`PKG_AGENT_MCP_SERVER_URL` 上的所有请求。`mcp_live_qemu_test` 先调
`mock_mcp_release()` 放行，后面的握手、取工具表、调桌面工具都走真链路。

用例查的是真东西：工具表里要有 `win__` 前缀的桌面工具和 `read_text_file`，
`win__displayinventory` 要回得出本机的显示器信息，`win__process` 要回得出
本机的进程列表，`win__screenshot` 要回成一句带下载地址的说明，`win__snapshot`
要回得出桌面结构，`win__app` 要真的把 QQ 启动起来，`user__audio` 要回成一条
带下载地址的音频条目，`web_search` 要回得出搜索服务给的结果。判据是 22 项检查
全部通过。

脚本最后还会按板端拿到的地址把图取回来，确认存下来的确实是 PNG，
字节数记在 `result_live.json` 的 `stored_image` 里。

真链路的工具表是网关给的，离线用例的工具表是替身给的，两边的名字不一样。
离线用例里有 22 个工具，2 个固定名字加 20 个模拟的桌面工具。真链路里是
11 个内置文件工具加 20 个 Windows-MCP 桌面工具，加 3 个用户工具，
再加 1 个联网搜索工具，一共 35 个。
装的 Windows-MCP 是 0.8.8 时多一个 `ControlStatus`，工具数变成 36 个。

## 9. 排错

**设备连不上网关。** 先在 PC 上 `curl -X POST http://127.0.0.1:8765/mcp -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}"`，
本机通、设备不通就是网络或防火墙问题。Windows 防火墙默认拦 Python 的入站连接，
第一次启动会弹窗，要选「允许访问」。

**终端里看不到启动日志。** Python 把输出缓冲在内存里，重定向到文件时日志要等进程结束才出现。
启动脚本里用了 `python -u`，一般不会遇到这个问题。

**握手失败。** 网关的日志会打印具体原因。设备端日志里出现 `mcp: server unreachable`，
说明连不上或者服务端不是 MCP；出现 `mcp: no JSON-RPC response for id N`，说明服务端回了
非 JSON 内容。

**服务端返回 chunked 编码。** webclient v2.2.0 识别不了 `Transfer-Encoding: chunked`，
会把响应体读到超时。本目录的网关用 `Content-Length` 回包，不受影响；换成别的 MCP 服务端
时，如果它只支持 chunked，需要在它前面加一层转换。FastMCP 系的 HTTP 传输默认就发
chunked，这类服务器只能经网关走。

**Windows-MCP 没接上。** 启动日志会说明原因。用启动脚本的话，装 uv 和装包是自动的，
日志里会先打印这两步，命令路径也会打出来。提示握手失败，说明 Windows-MCP 没能起来，
先在命令行手动跑一次看报错。第一次跑要下载包，多等一会儿。

**工具名冲突。** 同名工具会自动加前缀，内置文件工具用 `file__`，ODR 服务器用服务器名，
命令行服务器用 `win__`，用户工具用 `user__`。在 `tools/list` 的结果里能看到最终名字。

**用户工具没加载。** 启动日志里会打印一行「用户工具 N 个」。没有这行，说明目录里
没有可加载的工具，或者启动时加了 `--no-user-tools`。单个文件出错时日志里会打印
原因，其余文件照常注册。

**某个工具没注册。** 日志里会打印「工具 X 现在不能用，跳过」。工具的
`enabled` 是 `False` 就会这样，`--no-web-search` 走的是同一条路。

**图片没存下来。** 回包里是 `[image omitted by gateway: ...]` 就说明上传没成功。
网关的日志会打印原因，多半是文件服务器没起。先看 `curl http://127.0.0.1:8000/files`
通不通；不通就在 `resource/file_server` 里跑 `deploy.bat --start-only`。

**音频没存下来。** 回包里是 `[audio omitted by gateway: ...]` 就是同一个原因。
接了文件服务器时，外部 MCP 服务器的音频回包里是
`[audio saved: <地址> (audio/wav, N bytes)]`，用户工具的音频回包里是一条
`{"type": "audio", "url": "<地址>"}`。把地址在浏览器里打开能下载到那段音频。
下载回来的类型不对，多半是工具给的 `mimeType` 和实际数据不符，网关按 `mimeType`
定扩展名。

**用户工具报「没有配置文件服务器」。** 网关启动时带了 `--no-file-server`，
或者文件服务器没起。`to_content(image=...)` 要往文件服务器上传，没地方传就报错。

**联网搜索调不通。** 回包里是 `web_search 请求失败：...` 就说明没连上搜索服务。
先在 PC 上 `curl -m 10 "<搜索地址>?q=rt-thread&format=json"`，
本机通、网关不通就是地址写错了。默认地址是 SearXNG 的公开实例，
国内网络常常连不上，换一个能连的地址或者自己起一个 SearXNG。

**联网搜索回来的结果为空。** 搜索服务回了 JSON，但网关没找到结果。
多数是回包里没有链接字段，或者搜索服务回的不是 JSON。先看搜索服务自己回了什么，
再对一下第 7.1 节里对接方式那一列。

**工具表里没有 `web_search`。** 启动时加了 `--no-web-search`，
或者扫描 `tools/user_tools/` 时这个模块没加载成功，日志里会打印原因。
