# Agent API 文档

> 框架全部公开 API 参考，按模块分组。框架整体说明见 [framework.md](./framework.md)。

---

## 目录

1. [MessageHub —— 消息中心与消息数组](#1-messagehub--消息中心与消息数组)
2. [Chat —— LLM 通信](#2-chat--llm-通信)
3. [Context —— 上下文管理器](#3-context--上下文管理器)
4. [Tool —— 工具系统](#4-tool--工具系统)
5. [Channels —— 交互通道](#5-channels--交互通道)
6. [Utils —— 通用工具与多模态](#6-utils--通用工具与多模态)
7. [AgentConfig —— 运行时配置](#7-agentconfig--运行时配置)
8. [Prompt —— 系统提示词](#8-prompt--系统提示词)
9. [MSH 命令](#9-msh-命令)

---

## 1. MessageHub —— 消息中心与消息数组

**头文件**：`include/MessageHub.h`　**实现**：`src/MessageHub.c`

### 1.1 类型定义

```c
/* 消息类型 */
typedef enum MessageType { TYPE_TEXT, TYPE_AUDIO, TYPE_IMAGE, TYPE_VIDEO, TYPE_NULL } MessageType;

/* 单条消息 */
typedef struct MessageItem { MessageType message_type; char *content; } MessageItem, *MessageItem_t;

/* 不定长消息数组（自动扩容） */
typedef struct Messages { MessageItem_t message_ptr; int max_size; int current_size; } Messages, *Messages_t;

/* 消息中心 */
typedef struct MessageHub {
    rt_mailbox_t input_mailbox;      /* 输入邮箱（通道→主循环） */
    rt_mailbox_t output_mailbox;     /* 输出邮箱（主循环→通道） */
    rt_err_t (*put_message)(struct MessageHub *self, Messages_t message, rt_mailbox_t mb);
    Messages_t (*get_message)(struct MessageHub *self, rt_mailbox_t mb);
} MessageHub, *MessageHub_t;
```

邮箱常量：输入 `AgentInputMb`（容量 10）、输出 `AgentOutMb`（容量 10）。

### 1.2 消息中心接口

#### `MessageHub_t message_hub_create(void)`
创建消息中心（含两个邮箱）。失败返回 `RT_NULL`（部分创建失败会自动回滚）。

#### `void message_hub_destroy(MessageHub_t hub)`
销毁消息中心。**删除前排空两个邮箱并释放残留的 `Messages_t`**（防消息泄漏）。`hub` 为 `RT_NULL` 时安全返回。

#### `rt_err_t message_hub_put(MessageHub_t hub, Messages_t message, rt_mailbox_t mb)`
发送消息（入队指针）。**入队成功即移交所有权给接收方**，发送方不得再释放 `message`。返回 `RT_EOK` 成功。

#### `Messages_t message_hub_get(MessageHub_t hub, rt_mailbox_t mb)`
阻塞接收消息（`RT_WAITING_FOREVER`）。返回取到的 `Messages_t`，调用方负责释放。

#### `Messages_t message_hub_get_timeout(MessageHub_t hub, rt_mailbox_t mb, rt_int32_t timeout)`
带超时接收消息，供通道做**可中断的输出等待**（避免清理卡死）。
- 参数 `timeout`：tick 超时（`RT_WAITING_NO` 表示不等待）。
- 返回：超时/失败返回 `RT_NULL`，成功返回 `Messages_t`（调用方负责释放）。

### 1.3 消息数组接口

#### `Messages_t messages_create(int max_size)`
创建消息数组。`max_size` 为初始容量，`0` 表示延迟分配（首条 append 时扩容）。失败返回 `RT_NULL`。

#### `rt_err_t messages_append(Messages_t messages, MessageType message_type, const char *content)`
追加一条消息（内容内部 `strdup` 拷贝）。容量不足自动 +5 扩容。失败返回 `RT_ERROR`。

#### `void messages_destroy(Messages_t messages)`
释放消息数组及其全部内容。`messages` 为 `RT_NULL` 时安全返回。

#### `char *messages_get_content_idx(Messages_t messages, int idx)`
获取指定索引的消息内容指针（不拷贝）。越界返回 `RT_NULL`。

#### `MessageType messages_get_type_idx(Messages_t messages, int idx)`
获取指定索引的消息类型。越界返回 `TYPE_NULL`。

#### `char *get_agent_content_type(MessageType message_type)`
消息类型 → 字符串（`"text"` / `"audio_url"` / `"image_url"` / `"video_url"`）。

---

## 2. Chat —— LLM 通信

**头文件**：`include/chat.h`　**实现**：`src/chat.c`

### 2.1 类型定义

```c
typedef struct ChatResponse {
    char *reasoning;    /* 思考过程文本 */
    char *context;      /* 回答内容文本 */
    cJSON *tool_call;   /* 工具调用数组 [list[Dict]] */
} ChatResponse, *ChatResponse_t;
```

### 2.2 接口

#### `ChatResponse_t chat(cJSON *messages, cJSON *tools, int max_tokens, void (*on_reasoning)(const char *text), void (*on_tool_call)(const char *text), void (*on_context)(const char *text))`

发送聊天请求并流式读取 SSE 响应。

- `messages`：消息列表 cJSON 数组（内部 Duplicate，不取走所有权）。
- `tools`：工具定义 cJSON 数组，可传 `RT_NULL`。
- `max_tokens`：最大 token 数。
- `on_reasoning` / `on_tool_call` / `on_context`：流式回调，可传 `RT_NULL`。
- 返回：成功返回 `ChatResponse_t`（**必须 `chat_response_free` 释放**）；失败返回 `RT_NULL`。
- 内部：30s 网络超时；SSE 解析失败自动复位行缓冲。

#### `void chat_response_free(ChatResponse_t resp)`
释放聊天响应（reasoning / context / tool_call / 结构体）。`resp` 为 `RT_NULL` 时安全返回。

---

## 3. Context —— 上下文管理器

**头文件**：`include/context.h`　**实现**：`src/context.c`

### 3.1 类型定义

```c
typedef enum RoleType { ROLE_SYSTEM, ROLE_USER, ROLE_ASSISTANT, ROLE_TOOL } RoleType;

typedef struct Context {
    cJSON *message;  /* 消息列表 cJSON 数组 */
    rt_err_t (*build_system_prompt)(struct Context *self);
    rt_err_t (*append_user_message)(struct Context *self, Messages_t messages);
    rt_err_t (*append_assistant_message)(struct Context *self, ChatResponse_t resp);
    rt_err_t (*append_tool_message)(struct Context *self, const char *tool_call_id, Messages_t messages);
    rt_err_t (*clear_message)(struct Context *self);
    rt_err_t (*trim_context)(struct Context *self, int keep_rounds);
} Context, *Context_t;
```

### 3.2 接口

#### `char *get_role_string(RoleType type)`
角色枚举 → 字符串（`"system"` / `"user"` / `"assistant"` / `"tool"`）。

#### `Context_t agent_context_create(void)`
创建上下文管理器并自动构建系统提示词。失败返回 `RT_NULL`。

#### `void agent_context_destroy(Context_t context)`
销毁上下文管理器（释放消息数组与结构体）。`context` 为 `RT_NULL` 时安全返回。

#### 方法指针（通过 `Context_t` 调用）

| 方法 | 说明 |
|---|---|
| `build_system_prompt(self)` | 在消息列表头部加入 system 消息 |
| `append_user_message(self, messages)` | 追加用户消息（内容拷贝，`messages` 所有权仍归调用者） |
| `append_assistant_message(self, resp)` | 无 tool_calls 时追加正文；有 tool_calls 时追加含 tool_calls 的助手消息（内部 Duplicate） |
| `append_tool_message(self, tool_call_id, messages)` | 追加工具返回消息（绑定 `tool_call_id`） |
| `clear_message(self)` | 清空消息数组（不重建系统提示词，需另行调用 `build_system_prompt`） |
| `trim_context(self, keep_items)` | 保留系统提示词 + 最近 `keep_items` 条消息 |

---

## 4. Tool —— 工具系统

**头文件**：`include/tool_base.h`、`include/tool_func.h`　**实现**：`src/tool_base.c`、`src/tool_func.c`、`src/tools/*`

### 4.1 类型定义

```c
/* 工具返回值 */
typedef struct AgentToolRet {
    rt_err_t ret;
    Messages_t messages;
} AgentToolRet, *AgentToolRet_t;

/* 工具链表节点 */
typedef struct AgentToolList {
    cJSON *tool_obj;
    AgentToolRet ret;
    void (*execute_func)(cJSON *param, struct AgentToolList *node);
    struct AgentToolList *next;
} AgentToolList, AgentToolNode, *AgentToolNode_t;
```

工具执行函数约定：`void tool_xxx(cJSON *args, AgentToolNode_t node)`，结果写入 `node->ret.messages`（再次执行前内部会释放旧消息）。

### 4.2 接口（tool_base.h）

| 函数 | 说明 |
|---|---|
| `rt_err_t agent_tool_list_create(void)` | 创建工具链表头节点 |
| `rt_err_t append_tool(cJSON *tool_obj, void (*execute_func)(cJSON*, AgentToolNode_t))` | 追加工具节点（`tool_obj` 所有权在 `build_tools_json` 时移交 `AgentTools` 数组） |
| `cJSON *build_tools_json(void)` | 构建工具定义 JSON 数组（首次调用后复用） |
| `cJSON *get_agent_tools(void)` | 获取已构建的工具 JSON 数组 |
| `AgentToolNode_t search_agent_tool_node(cJSON *tool_obj)` | 按 `function.name` 查找工具节点 |
| `void free_agent_tool_node(AgentToolNode_t node)` | 释放节点自身与 `ret.messages`（**不释放** `tool_obj`，其归 `AgentTools` 数组所有） |
| `void agent_tool_list_destroy(void)` | 销毁整个工具链表 + `AgentTools` 数组 + 节点残留消息 |

### 4.3 接口（tool_func.h）

| 函数 | 说明 |
|---|---|
| `void init_tools(void)` | 注册内置工具（add / mul / compare），幂等（`init_flag` 保护） |
| `void agent_tools_cleanup(void)` | 清理全部工具并复位注册标志（由 `cleanup_agent` 调用，再次进入时 `init_tools` 重新注册） |

### 4.4 内置工具

| 工具 | 签名 | 参数 | 结果 |
|---|---|---|---|
| `tool_add` | `void tool_add(cJSON *args, AgentToolNode_t node)` | `a`, `b`（double） | `"计算结果: <a+b>"` |
| `tool_mul` | `void tool_mul(cJSON *args, AgentToolNode_t node)` | `a`, `b`（double） | `<a*b>` |
| `tool_compare` | `void tool_compare(cJSON *args, AgentToolNode_t node)` | `a`, `b`（double） | 大小比较文本 |

---

## 5. Channels —— 交互通道

**头文件**：`include/channels/*`　**实现**：`src/channels/*`

### 5.1 通道 ops 接口（`include/channels/AgentChannel.h`）

```c
typedef struct AgentChannelOps {
    int  (*init)(MessageHub_t hub, Context_t ctx);  /* 通道初始化：绑定 hub/context */
    void (*reset)(void);                            /* 通道清理：停止线程/释放句柄/置空缓存（幂等） */
} AgentChannelOps;
```

`AgentLoop` 通过全局 `g_channel_ops`（`AGENT_CHANNEL_OPS` 宏，`include/channels/AgentChannels.h` 单点选择）统一调用 init/reset，调用点无 `#ifdef`。

### 5.2 CLI 通道（`include/channels/CLI.h`，宏 `PKG_AGENT_CLI_CHANNEL`）

| 函数 | 说明 |
|---|---|
| `int agent_cli_channel(MessageHub_t message_hub, Context_t context)` | 通道初始化：创建 readline 线程与信号量；hub/context 为 NULL 返回 `-RT_EINVAL` |
| `void agent_cli_stop(void)` | 停止线程并等待退出，随后释放信号量与句柄、置空全局（幂等） |
| `extern AgentChannelOps agent_cli_ops` | ops 实例 `{ agent_cli_channel, agent_cli_stop }` |

### 5.3 WebNet 通道（`include/channels/AgentWebChannel.h`，宏 `PKG_AGENT_WEBNET_CHANNEL`）

| 函数 | 说明 |
|---|---|
| `void webnet_agent_mode(MessageHub_t hub, Context_t ctx)` | 注册 CGI（`chat` / `config` / `get_config`）并启动 webnet |
| `void agent_webnet_reset(void)` | 置空缓存的 hub/context 指针（幂等）；CGI 带 NULL 检查，优雅失败 |
| `extern AgentChannelOps agent_webnet_ops` | ops 实例 `{ webnet_agent_channel_init, agent_webnet_reset }` |

WebNet HTTP 接口（会话内）：`POST /chat`（`message` / `stream` / `reset` 字段，支持 SSE 流式）、`POST /config`、`GET /get_config`。

### 5.4 Debug 通道（`include/channels/AgentDebugChannel.h`，宏 `PKG_AGENT_DEBUG_CHANNEL`）

| 函数 | 说明 |
|---|---|
| `int agent_debug_channel(MessageHub_t message_hub, Context_t context)` | 绑定 hub/context（无条件刷新缓存指针） |
| `void agent_debug_reset(void)` | 置空缓存的 hub/context 指针（幂等） |
| `extern AgentChannelOps agent_debug_ops` | ops 实例 `{ agent_debug_channel, agent_debug_reset }` |

---

## 6. Utils —— 通用工具与多模态

**头文件**：`include/utils.h`　**实现**：`src/utils.c`

### 6.1 消息/JSON 工具

| 函数 | 说明 |
|---|---|
| `cJSON *format_text(const char *text)` | 生成 `{ "type": "text", "text": ... }` |
| `cJSON *create_content_array(void)` | 创建空内容数组 |
| `cJSON *to_content(Messages_t messages)` | Messages → API content：非多模态返回 `{content}`，多模态返回 `[{type,...},...]`（音频/图片/视频自动上传） |
| `void print_reasoning(const char *text)` / `print_tool_call(const char *text)` / `print_context(const char *text)` | 回调打印函数 |

### 6.2 工具 JSON 构建

| 函数 | 说明 |
|---|---|
| `cJSON *create_tool_item(const char *func_name, const char *desc, cJSON *params_obj)` | 生成工具项 `{type:"function", function:{name, description, parameters}}` |
| `cJSON *create_param_obj(cJSON *props_obj, cJSON *required_arr)` | 生成 `{type:"object", properties, required}` |
| `void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc)` | 向属性对象添加单个属性 |

### 6.3 多模态文件服务器（`PKG_AGENT_MULTIMODAL_ENABLE`）

```c
typedef enum {
    CMD_AGENT_FILE_UPLOAD, CMD_AGENT_FILE_DOWNLOAD, CMD_AGENT_FILE_EXIT,
    CMD_AGENT_FILE_EXIT_COMPLETE, CMD_AGENT_FILE_URL
} file_op_type_t;

typedef struct { file_op_type_t op; char path[128]; char file_id[128]; } file_op_req;
```

| 函数 | 说明 |
|---|---|
| `rt_thread_t agent_file_op_init(void)` | 创建文件操作线程与消息队列 |
| `void agent_file_op_deinit(void)` | 发送退出命令、等待线程退出并释放队列/信号量 |
| `char *agent_up_load(const char *path)` | 上传文件，返回 URL（静态缓冲，调用方需及时拷贝） |
| `rt_err_t down_load(const char *path, const char *file_id)` | 下载文件到本地路径 |
| `rt_mq_t get_agent_file_op_mq_input(void)` / `get_agent_file_op_mq_output(void)` | 获取文件操作消息队列 |

---

## 7. AgentConfig —— 运行时配置

**头文件**：`config/AgentConfig.h`　**实现**：`config/AgentConfig.c`

### 7.1 类型定义

```c
typedef struct agent_runtime_config {
    char api_key[AGENT_CFG_API_KEY_MAX];    /* 128 */
    char model_name[AGENT_CFG_MODEL_MAX];   /* 128 */
    char api_url[AGENT_CFG_API_URL_MAX];    /* 128 */
    rt_bool_t is_configured;
} rt_align(RT_ALIGN_SIZE) agent_runtime_config_t;
```

### 7.2 接口

| 函数 | 说明 |
|---|---|
| `agent_runtime_config_t *agent_config_get(void)` | 获取运行时配置指针（静态存储，无堆分配） |
| `void agent_config_set(const char *api_key, const char *model_name, const char *api_url)` | 热更新运行时配置（参数可部分为 NULL，有边界检查） |
| `const char *get_dynamic_agent_api_key(void)` | 优先返回运行时配置，否则返回编译期宏 `PKG_AGENT_API_KEY` |
| `const char *get_dynamic_agent_model_name(void)` | 同上，回退 `PKG_AGENT_MODEL_NAME` |
| `const char *get_dynamic_agent_api_url(void)` | 同上，回退 `PKG_AGENT_API_URL` |

---

## 8. Prompt —— 系统提示词

**头文件**：`include/prompt.h`　**实现**：`src/prompt.c`

| 函数 | 说明 |
|---|---|
| `char *get_system_prompt(void)` | 获取系统提示词（静态字符串，勿释放） |

---

## 9. MSH 命令

| 命令 | 来源 | 说明 |
|---|---|---|
| `main_loop_entry` | AgentLoop | 启动代理主循环（进入对话） |
| `cleanup_agent_entry` | AgentLoop | 停止并清理代理（退出对话，释放全部资源） |
| `init_tools` | tool_func | 手动注册工具（通常由 `init_agent` 自动调用） |
| `agent_send_message <type> <content>` | Debug 通道 | 向代理发送一条消息（0=text，1=audio，2=image，3=video） |
| `show_llm_config` | WebNet 通道 | 显示当前 LLM 配置与通道状态 |

---

## 10. 变更记录（本次会话）

| 变更 | 说明 |
|---|---|
| `message_hub_get_timeout()` | 新增：可中断输出等待，解决清理卡死 |
| `message_hub_destroy()` | 增强：销毁前排放残留消息 |
| `agent_tool_list_destroy()` / `agent_tools_cleanup()` | 新增：工具系统整体释放 |
| `free_agent_tool_node()` | 修正：不再释放已移交 `AgentTools` 的 `tool_obj` |
| `AgentChannelOps` + `agent_*_ops` | 新增：通道 init/reset 函数指针统一分发 |
| `AGENT_CHANNEL_OPS` | 新增单点宏选择（替代 `AGENT_CHANNEL_IMPL`） |
| `agent_debug_reset()` / `agent_webnet_reset()` | 新增：通道缓存指针清理 |
| `agent_cli_stop()` | 增强：停止后释放句柄与信号量 |
| 输入消息所有权 | 修正：通道不再在入队后释放，由 main_loop 消费后释放（消除 use-after-free） |
| `chat()` | SSE 解析失败复位行缓冲；新增 30s 网络超时 |
| `LOG_TAG`/`LOG_LVL`/`<ulog.h>` | 从头文件迁移至各 .c 文件（消除重定义警告） |
