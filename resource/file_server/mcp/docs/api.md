# 网关接口

网关只提供 HTTP，一个端点：`POST /mcp`。请求是 JSON-RPC 2.0，回包也是 JSON。
设备的 MCP 客户端认不了 chunked，所以回包一定带 `Content-Length`。

## 1. JSON-RPC 方法

| 方法 | 参数 | 回包 |
|---|---|---|
| `initialize` | `protocolVersion`、`capabilities`、`clientInfo` | 协议版本、能力、服务端信息 |
| `notifications/initialized` | 无 | 空包，HTTP 202 |
| `ping` | 无 | 空对象 |
| `tools/list` | 无 | 工具清单，每项含 `name`、`description`、`inputSchema` |
| `tools/call` | `name`、`arguments` | `content` 数组和 `isError` |

`initialize` 的回包带 `Mcp-Session-Id` 头。客户端要在后面的请求里带上这个头，
网关按它认会话。

网关起一个事件循环，工具的执行都在这个循环里跑。并发进来的调用互不阻塞，
一个工具在等网络的时候，别的工具照样能调。单个工具最多跑 `--call-timeout`
秒，默认 60 秒，超了按失败返回。

## 2. 工具清单

工具只有一种：`tools/base.py` 里的 `Tool`。每个工具都是它的子类。名字在
`tools/list` 里是最终名字，板端收到后再加一层 `mcp_` 前缀。

| 来源 | 对外名字 | 工具数 | 说明 |
|---|---|---|---|
| 内置文件工具 | 工具原名 | 11 | 名字与 Windows 文件资源管理器 MCP 连接器一致，代码在 `tools/file/` |
| 联网搜索 | `web_search` | 1 | 搜索工具，代码在 `tools/user_tools/web_search_tool.py` |
| 用户工具 | `user__` 加工具名 | 按目录 | 用户自己的工具，代码在 `tools/user_tools/` |
| 命令行服务器 | 服务器名加 `__` 加工具名 | 看服务器 | 命令行拉起的 MCP 服务器，Windows-MCP 是 20 到 21 个 |
| ODR 服务器 | 服务器名加 `__` 加工具名 | 看注册表 | Windows 11 的 ODR 里注册的服务器 |

用户工具和联网搜索放在同一个目录里。区别在类里写的 `namespace`：写了空串就
用工具原名，没写就加 `user__` 前缀。

工具名有两个前缀，分开用：

| 前缀 | 什么时候用 | 值 |
|---|---|---|
| namespace | 一直带在名字前面，用来区分来源 | 见上表的「对外名字」 |
| prefix | 只在和别的工具撞名时用上 | `file__`、`user__`、`win__`、服务器名、`web__` |

撞名的那个工具改用 prefix 加原名，还是重就再加序号。`tools/list` 里能看到最终名字。

## 3. 内置文件工具

| 工具 | 参数 | 说明 |
|---|---|---|
| `get_file_details` | `filePath` | 文件的属性 |
| `get_directories` | `path` | 目录下的子目录 |
| `read_text_file` | `filePath` | 读文本内容 |
| `read_file` | `filePath` | 按资源读内容 |
| `search_files` | `path`、`pattern` | 按名字找文件 |
| `create_directory` | `path` | 建目录 |
| `create_text_file` | `filePath`、`content` | 写文本文件 |
| `edit_text_file` | `filePath`、`oldText`、`newText` | 改文件里的文本 |
| `move_file` | `source`、`destination` | 移动或改名 |
| `zip_folder` | `source`、`destination` | 打包目录 |
| `unzip_folder` | `source`、`destination` | 解包 |

路径都在 `--root` 给的根目录里，目录外的路径一律拒绝。

## 4. 联网搜索工具

```json
{
  "name": "web_search",
  "description": "Search the web and return titles, links and snippets. ...",
  "inputSchema": {
    "type": "object",
    "properties": {
      "query": {"type": "string", "title": "what to search for"},
      "count": {"type": "integer", "title": "how many results to return"}
    },
    "required": ["query"]
  }
}
```

| 参数 | 说明 |
|---|---|
| `query` | 搜什么，必填 |
| `count` | 几条结果，最少 1 条，最多 10 条，默认 5 条 |

回包是一段 JSON 文本：

```json
{
  "query": "RT-Thread",
  "count": 3,
  "results": [
    {"title": "标题", "url": "https://example.com", "snippet": "摘要"}
  ]
}
```

搜索服务不可达、地址没配、`query` 缺失都按失败返回，`isError` 是 true，
原因写在 `content` 的文字里。

## 5. 媒体条目

回包的 `content` 数组里有三种条目：

| 类型 | 形态 | 谁给的 |
|---|---|---|
| 文字 | `{"type": "text", "text": "..."}` | 所有工具 |
| 图片 | `{"type": "image", "url": "http://..."}` | 用户工具用 `to_content(image=...)` |
| 音频 | `{"type": "audio", "url": "http://..."}` | 用户工具用 `to_content(audio=...)` |

外部 MCP 服务器给的图片是 base64，网关把它存到文件服务器，回包里换成一句说明：

```
[image saved: http://127.0.0.1:8000/download/<file_id> (image/png, 516817 bytes)]
```

板端只拼文字，图片和音频渲染成 `[image: 地址]` 和 `[audio: 地址]`。

## 6. 限制

| 项 | 值 | 说明 |
|---|---|---|
| 回包上限 | 32768 字节 | 板端的 `PKG_AGENT_MCP_RESP_BUFSZ` |
| 文字上限 | 24000 字节 | 网关的 `--max-text-bytes`，要小于板端上限 |
| 工具数上限 | 40 | 板端的 `PKG_AGENT_MCP_MAX_TOOLS` |
| 描述长度 | 1280 字节 | 板端每个工具的描述缓冲区 |
| 调用超时 | 60 秒 | 网关的 `--call-timeout` |

超出上限时网关自己裁。文字截断到上限，结尾标出丢了多少字符。
图片和音频先落盘再换成地址，base64 不上线。
