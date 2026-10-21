# Agent API 文档

本文按模块列出公开接口。框架结构与数据流见 [framework.md](./framework.md)。

## 目录

1. [MessageHub：消息中心与消息数组](#1-messagehub消息中心与消息数组)
2. [Chat：LLM 通信](#2-chatllm-通信)
3. [Context：上下文管理器](#3-context上下文管理器)
4. [Tool：工具系统](#4-tool工具系统)
5. [Channels：交互通道](#5-channels交互通道)
6. [Utils：通用工具、多模态与 STT](#6-utils通用工具多模态与-stt)
7. [AgentAudio：音频驱动](#7-agentaudio音频驱动)
8. [AgentConfig：运行时配置](#8-agentconfig运行时配置)
9. [Prompt：系统提示词](#9-prompt系统提示词)
10. [MSH 命令](#10-msh-命令)
11. [已知差异](#11-已知差异)

## 1. MessageHub：消息中心与消息数组

头文件：`include/MessageHub.h` 实现：`src/MessageHub.c`

### 1.1 类型

```c
typedef enum MessageType
{
    TYPE_TEXT,
    TYPE_AUDIO,
    TYPE_IMAGE,
    TYPE_VIDEO,
    TYPE_NULL
} MessageType;

typedef struct MessageItem
{
    MessageType message_type;
    char *content;
} MessageItem, *MessageItem_t;

typedef struct Messages
{
    MessageItem_t message_ptr;
    int max_size;
    int current_size;
} Messages, *Messages_t;

typedef struct MessageHub
{
    rt_mailbox_t input_mailbox;
    rt_mailbox_t output_mailbox;
    rt_err_t (*put_message)(struct MessageHub *self, Messages_t message, rt_mailbox_t mb);
    Messages_t (*get_message)(struct MessageHub *self, rt_mailbox_t mb);
} MessageHub, *MessageHub_t;
```

邮箱：输入 `AgentInputMb`，输出 `AgentOutMb`，容量都是 10。

### 1.2 消息中心

#### `MessageHub_t message_hub_create(void)`

创建消息中心与两个邮箱。失败返回 `RT_NULL`，已创建的资源会回滚。

#### `void message_hub_destroy(MessageHub_t hub)`

销毁消息中心。销毁前排空两个邮箱并释放残留消息。`hub` 为 `RT_NULL` 时直接返回。

#### `rt_err_t message_hub_put(MessageHub_t hub, Messages_t message, rt_mailbox_t mb)`

把消息指针放入邮箱。成功返回 `RT_EOK`，所有权移交接收方。入队后发送方不得再释放该消息。

#### `Messages_t message_hub_get(MessageHub_t hub, rt_mailbox_t mb)`

阻塞接收，等待时间为 `RT_WAITING_FOREVER`。返回的消息由调用方释放。

#### `Messages_t message_hub_get_timeout(MessageHub_t hub, rt_mailbox_t mb, rt_int32_t timeout)`

带超时接收，供通道做可中断的输出等待。

- `timeout` 单位为 tick，`RT_WAITING_NO` 表示立即返回。
- 超时返回 `RT_NULL`，成功返回消息，调用方负责释放。

### 1.3 消息数组

#### `Messages_t messages_create(int max_size)`

创建消息数组。`max_size` 为初始容量，传 0 表示首次追加时再分配。失败返回 `RT_NULL`。

#### `rt_err_t messages_append(Messages_t messages, MessageType message_type, const char *content)`

追加一条消息，内容会复制一份。容量不足时按 5 条递增。失败返回 `RT_ERROR`。

#### `void messages_destroy(Messages_t messages)`

释放数组及全部内容。`messages` 为 `RT_NULL` 时直接返回。

#### `char *messages_get_content_idx(Messages_t messages, int idx)`

返回指定索引的内容指针，不复制。越界返回 `RT_NULL`。

#### `MessageType messages_get_type_idx(Messages_t messages, int idx)`

返回指定索引的消息类型。越界返回 `TYPE_NULL`。

#### `char *get_agent_content_type(MessageType message_type)`

消息类型转字符串：`text`、`audio_url`、`image_url`、`video_url`。内部数组只有 4 项，传入 `TYPE_NULL` 会越界读取，不要这样调用。

## 2. Chat：LLM 通信

头文件：`include/chat.h` 实现：`src/chat.c`

### 2.1 类型

```c
typedef struct ChatResponse
{
    char *reasoning;
    char *context;
    cJSON *tool_call;
} ChatResponse, *ChatResponse_t;
```

- `reasoning`：思考文本。
- `context`：回答正文。
- `tool_call`：工具调用数组，元素为 OpenAI 格式的 tool call 对象。

### 2.2 接口

#### `ChatResponse_t chat(cJSON *messages, cJSON *tools, int max_tokens, void (*on_reasoning)(const char *text), void (*on_tool_call)(const char *text), void (*on_context)(const char *text))`

发送请求并流式读取 SSE 响应。

- `messages`：消息数组，函数内部复制，不取走所有权。
- `tools`：工具定义数组，可传 `RT_NULL`。
- `max_tokens`：最大 token 数。主循环传 32 × 1024。
- 回调都在流式解析过程中调用，可传 `RT_NULL`。
- `on_tool_call` 在响应结束后收到完整的工具调用 JSON。
- 成功返回 `ChatResponse_t`，必须用 `chat_response_free()` 释放；失败返回 `RT_NULL`。
- 网络超时为 30 s。

解析规则：

- 只处理以 `data: ` 开头的行，遇到 `[DONE]` 结束。
- 思考文本优先取 `delta.reasoning_content`，没有时取 `delta.reasoning`。
- 正文取 `delta.content`。
- 工具调用按 `index` 合并，`id` 与 `name` 取第一个非空值，`arguments` 逐片拼接。
- 缺失 `id` 的调用用 `call_<index>` 补齐，空参数改为 `{}`，没有函数名的调用丢弃。

#### `void chat_response_free(ChatResponse_t resp)`

释放思考文本、正文、工具调用数组与结构体。`resp` 为 `RT_NULL` 时直接返回。

## 3. Context：上下文管理器

头文件：`include/context.h` 实现：`src/context.c`

### 3.1 类型

```c
typedef enum RoleType
{
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} RoleType;

typedef struct Context
{
    cJSON *message;
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

角色枚举转字符串：`system`、`user`、`assistant`、`tool`。

#### `Context_t agent_context_create(void)`

创建上下文并构建系统提示词。失败返回 `RT_NULL`。

#### `void agent_context_destroy(Context_t context)`

释放消息数组与结构体。`context` 为 `RT_NULL` 时直接返回。

#### 方法指针

| 方法 | 说明 |
|---|---|
| `build_system_prompt(self)` | 在数组头部加入 system 消息 |
| `append_user_message(self, messages)` | 追加 user 消息，内容复制，调用方保留 `messages` 所有权 |
| `append_assistant_message(self, resp)` | 无工具调用时追加正文；有工具调用时追加 `content` 为空、带 `tool_calls` 的 assistant 消息 |
| `append_tool_message(self, tool_call_id, messages)` | 追加 `role=tool` 消息，只拼接 `TYPE_TEXT` 内容，结果是一个字符串 |
| `clear_message(self)` | 删除全部消息并新建空数组，不重建系统提示词 |
| `trim_context(self, keep_rounds)` | 保留 system 消息和最近 `keep_rounds` 条消息 |

工具结果只支持文本。图片、音频等多模态内容不能放进 `role=tool` 消息。

`trim_context` 的参数名是 `keep_rounds`，实际按消息条数裁剪，不是按对话轮次。它也可能把 assistant 消息与其后的 tool 消息拆开。保留条数过小时，下一轮请求可能被接口以 400 拒绝。

## 4. Tool：工具系统

头文件：`include/tool_base.h`、`include/tool_func.h` 实现：`src/tool_base.c`、`src/tool_func.c`、`src/tools/*`

### 4.1 类型

```c
typedef struct AgentToolRet
{
    rt_err_t ret;
    Messages_t messages;
} AgentToolRet, *AgentToolRet_t;

typedef struct AgentToolList
{
    cJSON *tool_obj;
    AgentToolRet ret;
    void (*execute_func)(cJSON *param, struct AgentToolList *node);
    struct AgentToolList *next;
} AgentToolList, AgentToolNode, *AgentToolNode_t;
```

工具函数签名：`void tool_xxx(cJSON *args, AgentToolNode_t node)`。结果写入 `node->ret.messages`。

### 4.2 接口

头文件 `tool_base.h`：

| 函数 | 说明 |
|---|---|
| `rt_err_t agent_tool_list_create(void)` | 创建工具链表头节点 |
| `rt_err_t append_tool(cJSON *tool_obj, void (*execute_func)(cJSON *, AgentToolNode_t))` | 追加工具节点 |
| `cJSON *build_tools_json(void)` | 构建工具定义数组，首次调用后复用 |
| `cJSON *get_agent_tools(void)` | 获取已构建的工具定义数组 |
| `AgentToolNode_t search_agent_tool_node(cJSON *tool_obj)` | 按 `function.name` 查找节点 |
| `void free_agent_tool_node(AgentToolNode_t node)` | 只释放节点与结果消息，不释放 `tool_obj`；工具定义由 `AgentTools` 数组统一释放 |
| `void agent_tool_list_destroy(void)` | 释放整个链表、工具数组与残留消息 |

头文件 `tool_func.h`：

| 函数 | 说明 |
|---|---|
| `void init_tools(void)` | 注册内置工具，重复调用不会重复注册 |
| `void agent_tools_cleanup(void)` | 清理工具并复位注册标志，再次进入时重新注册 |

### 4.3 内置工具

| 工具 | 参数 | 结果 |
|---|---|---|
| `add` | `a`, `b`（number） | 两数之和 |
| `mul` | `a`, `b`（number） | 两数之积 |
| `compare` | `a`, `b`（number） | 大小比较文本 |

### 4.4 MQTT 工具

头文件：`include/tools/tool_mqtt/tool_mqtt.h`
实现：`src/tools/tool_mqtt/`
使用说明：[tool_mqtt.md](./tool_mqtt.md)

启用 `PKG_AGENT_TOOL_MQTT_ENABLE` 后注册 7 个工具：

| 工具 | 参数 | 说明 |
|---|---|---|
| `mqtt_connect` | `timeout_ms?`（number） | 启动并连接 broker，返回连接状态 |
| `mqtt_disconnect` | 无 | 断开连接并释放工作线程，订阅意图保留 |
| `mqtt_publish` | `message`（string，必填），`topic?`（string） | 发布消息，省略话题时使用默认发布话题 |
| `mqtt_subscribe` | `action`（subscribe / unsubscribe / list），`topic?`，`mode?`（filter / auto / poll） | 订阅、退订或列出话题 |
| `mqtt_receive` | `max?`（number），`topic?`（string） | 取出缓存消息，默认最多 8 条，受 `PKG_AGENT_TOOL_MQTT_POLL_DEPTH` 限制 |
| `mqtt_rule` | `action`（set / clear / list），`topic?`，`field?`，`op?`，`value?` | 设置、清除或列出阈值规则 |
| `mqtt_history` | `topic?`（string），`max?`（number） | 汇总近期消息与数值统计，只读 |

投递模式：

| 模式 | 行为 |
|---|---|
| `filter` | 只有报文像咨询或命中阈值规则时才注入 agent，其余留在本地 |
| `auto` | 每条消息都注入 agent |
| `poll` | 不自动注入，由 `mqtt_receive` 取用 |

### 4.5 MQTT 应用层接口

| 函数 | 说明 |
|---|---|
| `rt_err_t mqtt_tool_add_topic(const char *topic, mqtt_deliver_mode_t mode)` | 增加或更新订阅，客户端未启动时自动启动 |
| `rt_err_t mqtt_tool_remove_topic(const char *topic)` | 删除订阅，由接收线程异步退订 |
| `int mqtt_tool_topic_count(void)` | 当前有效订阅数 |
| `rt_err_t mqtt_tool_start(void)` | 启动客户端，幂等 |
| `void mqtt_tool_stop(void)` | 停止客户端并释放接收线程与队列，幂等 |
| `rt_bool_t mqtt_tool_is_started(void)` | 客户端是否已启动 |
| `rt_err_t mqtt_tool_publish(const char *topic, const char *payload, char *out, rt_size_t out_size)` | 主动发布，自动启动并连接 broker |
| `rt_bool_t mqtt_tool_topic_latest(const char *topic_filter, double *value_out, rt_tick_t *tick_out)` | 读取订阅话题的最新数值，只读 |
| `const char *mqtt_tool_conn_state(void)` | 返回 `stopped`、`connecting` 或 `connected` |
| `rt_bool_t mqtt_tool_ensure_connected(char *out, rt_size_t out_size)` | 确保已启动并已连接，返回是否成功 |

`mqtt_tool_publish()` 与 LLM 调用的 `mqtt_publish` 走同一条发布路径，受 100 ms 最小发布间隔限制。

`mqtt_tool_topic_latest()` 的查找规则：

- `topic_filter` 与订阅记录精确匹配；传 `RT_NULL` 或空字符串时取任意话题。
- 读取该话题最新一条历史消息。
- 优先解析 JSON 中的 `val` 字段；没有时按整段报文解析数值。
- 不消费历史，可重复调用。

### 4.6 Agent 运行时访问器

头文件：`include/AgentRuntime.h`

| 函数 | 说明 |
|---|---|
| `MessageHub_t agent_get_message_hub(void)` | 当前消息中心，agent 未运行时返回 `RT_NULL` |
| `rt_bool_t agent_is_running(void)` | 主循环是否正在运行 |
| `rt_bool_t agent_is_busy(void)` | 是否正在处理一次用户请求，注入方可据此错峰 |

### 4.7 文本注入

头文件：`include/utils.h`

| 函数 | 说明 |
|---|---|
| `rt_err_t agent_inject_text(const char *text)` | 把一段非空文本作为 user 消息交给 agent |

成功返回 `RT_EOK`，消息所有权移交主循环。agent 未运行、消息中心未就绪或输入邮箱已满时返回 `RT_ERROR`，调用方需要自行缓存或丢弃。

```c
#include "utils.h"

if (agent_inject_text("topic: device/1/data\npayload: {\"temp\":42}") != RT_EOK)
{
    /* agent 未运行或邮箱已满 */
}
```

需要错峰时先判断 `agent_is_busy()`。

## 5. Channels：交互通道

头文件：`include/channels/*` 实现：`src/channels/*`

### 5.1 通道接口

```c
typedef struct AgentChannelOps
{
    int  (*init)(MessageHub_t hub, Context_t ctx);
    void (*reset)(void);
} AgentChannelOps;
```

`AgentLoop` 通过 `AGENT_CHANNEL_OPS` 选择通道，选择点在 `include/channels/AgentChannels.h`。优先级为 LVGL → CLI → WebNet → Debug，未配置时返回 `RT_NULL`。

### 5.2 CLI 通道

宏：`PKG_AGENT_CLI_CHANNEL`

| 函数 | 说明 |
|---|---|
| `int agent_cli_channel(MessageHub_t hub, Context_t ctx)` | 创建 readline 线程与信号量，参数为空返回 `-RT_EINVAL` |
| `void agent_cli_stop(void)` | 停止线程，释放信号量与句柄，幂等 |
| `extern AgentChannelOps agent_cli_ops` | `{ agent_cli_channel, agent_cli_stop }` |

按 CTRL+D（0x04）退出 CLI。

### 5.3 WebNet 通道

宏：`PKG_AGENT_WEBNET_CHANNEL`

| 函数 | 说明 |
|---|---|
| `void webnet_agent_mode(MessageHub_t hub, Context_t ctx)` | 注册 CGI 并启动 webnet |
| `void agent_webnet_reset(void)` | 置空缓存的 hub 与 ctx，幂等 |
| `extern AgentChannelOps agent_webnet_ops` | `{ webnet_agent_channel_init, agent_webnet_reset }` |

CGI：

| 接口 | 说明 |
|---|---|
| `POST /cgi-bin/chat` | 字段 `message`、`stream`、`reset`；`stream` 为真时返回 SSE |
| `POST /cgi-bin/config` | 更新运行时配置 |
| `GET /cgi-bin/get_config` | 读取当前配置 |

SSE 事件依次为 `delta`、`final`、`done`。输出等待超时 30 s，失败返回 502。

### 5.4 Debug 通道

宏：`PKG_AGENT_DEBUG_CHANNEL`

| 函数 | 说明 |
|---|---|
| `int agent_debug_channel(MessageHub_t hub, Context_t ctx)` | 绑定 hub 与 ctx |
| `void agent_debug_reset(void)` | 置空缓存指针，幂等 |
| `extern AgentChannelOps agent_debug_ops` | `{ agent_debug_channel, agent_debug_reset }` |

通过 `agent_send_message <type> <content>` 注入消息，类型 0–4 对应 text、audio、image、video、null。命令会等待输出最多 30 s，超时返回 -1。

### 5.5 LVGL 通道

宏：`PKG_AGENT_LVGL_CHANNEL`
头文件：`include/channels/AgentLvglChannel.h`

| 函数 | 说明 |
|---|---|
| `int agent_lvgl_channel(MessageHub_t hub, Context_t ctx)` | 保存 hub，不使用 ctx |
| `void agent_lvgl_channel_reset(void)` | 清空 hub，幂等 |
| `MessageHub_t agent_lvgl_channel_hub(void)` | 取当前 hub，未初始化时返回 `RT_NULL` |
| `extern AgentChannelOps agent_lvgl_ops` | `{ agent_lvgl_channel, agent_lvgl_channel_reset }` |

包内只有接口，`agent_lvgl_ops` 由应用实现。本仓库示例在 `applications/lvgl/demo/lv_home_agent.c`：用 `message_hub_get_timeout()` 轮询输出，通过 MSH 命令启动 agent。

## 6. Utils：通用工具、多模态与 STT

头文件：`include/utils.h` 实现：`src/utils.c`

### 6.1 文本与 JSON

| 函数 | 说明 |
|---|---|
| `cJSON *format_text(const char *text)` | 生成 `{ "type": "text", "text": ... }` |
| `cJSON *create_content_array(void)` | 创建空内容数组 |
| `cJSON *to_content(Messages_t messages)` | 把消息数组转成 API content |
| `void print_reasoning(const char *text)` | 打印思考文本，首次打印时加 `[thinking]` 前缀 |
| `void print_tool_call(const char *text)` | 打印工具调用 |
| `void print_context(const char *text)` | 打印正文，并复位思考前缀状态 |

`to_content()` 的行为：

- 未启用多模态：返回字符串内容。
- 文本：`{type: "text", text: ...}`。
- 图片、视频：上传后生成 `image_url` 或 `video_url`。
- 音频：
  - 启用 STT：转写后删除本地文件；成功时生成文本项，失败时生成提示文本。
  - 未启用 STT：上传后生成 `audio_url`。
- 上传失败的消息跳过。

### 6.2 工具 JSON 构建

| 函数 | 说明 |
|---|---|
| `cJSON *create_tool_item(const char *func_name, const char *desc, cJSON *params_obj)` | 生成工具定义 |
| `cJSON *create_param_obj(cJSON *props_obj, cJSON *required_arr)` | 生成 `{type:"object", properties, required}` |
| `void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc)` | 添加一个属性 |

### 6.3 本地语音转写

仅在 `PKG_AGENT_STT_ENABLE` 时编译。内部函数 `agent_stt_transcribe(const char *path)` 在 `to_content()` 处理音频时调用，没有公开头文件声明。

- 向 `PKG_AGENT_STT_URL` 发送 multipart POST。
- 连接建立后把收发超时放宽到 120 s，首次加载 whisper 模型可能耗时较久。
- 解析 `{"code":0,"data":{"text":"..."}}`。
- 响应缓冲为静态 1024 字节。
- 失败返回 `RT_NULL`，由调用方生成提示文本。

文件服务器端的 `/stt` 接口见 [resource/file_server/README.md](../resource/file_server/README.md)。

### 6.4 多模态文件操作

启用 `PKG_AGENT_MULTIMODAL_ENABLE` 后可用：

```c
typedef enum
{
    CMD_AGENT_FILE_UPLOAD,
    CMD_AGENT_FILE_DOWNLOAD,
    CMD_AGENT_FILE_EXIT,
    CMD_AGENT_FILE_EXIT_COMPLETE,
    CMD_AGENT_FILE_URL
} file_op_type_t;

typedef struct
{
    file_op_type_t op;
    char path[128];
    char file_id[128];
} file_op_req;
```

| 函数 | 说明 |
|---|---|
| `rt_thread_t agent_file_op_init(void)` | 创建文件操作线程与两个消息队列 |
| `void agent_file_op_deinit(void)` | 发送退出命令，等待线程退出，释放队列与信号量 |
| `char *agent_up_load(const char *path)` | 上传文件，返回静态 128 字节 URL 缓冲，调用方需及时复制 |
| `rt_err_t agent_down_load(const char *path, const char *file_id)` | 下载文件，参数顺序是保存路径、文件 ID |
| `rt_mq_t get_agent_file_op_mq_input(void)` | 取输入消息队列 |
| `rt_mq_t get_agent_file_op_mq_output(void)` | 取输出消息队列 |

文件操作线程名为 `fileop_wk`，栈大小由 `PKG_AGENT_FILE_OP_THREAD_SIZE` 配置，优先级 9。

`utils.h` 的声明与实际符号存在差异，见第 11 节。

## 7. AgentAudio：音频驱动

头文件：`include/driver/audio/AgentAudio.h` 实现：`src/driver/audio/AgentAudio.c`

启用宏：`PKG_AGENT_DRIVER_AUDIO`

| 函数 | 说明 |
|---|---|
| `int agent_driver_audio_play(const char *path)` | 播放 WAV，`wavplay` 内部自带线程，调用不阻塞 |
| `int agent_driver_audio_stop(void)` | 停止播放 |
| `int agent_driver_audio_pause(void)` | 暂停播放 |
| `int agent_driver_audio_resume(void)` | 继续播放 |
| `int agent_driver_audio_set_volume(int vol)` | 设置音量 |
| `int agent_driver_audio_record(const char *path)` | 开始录音，内部线程执行，不阻塞调用者 |
| `int agent_driver_audio_record_stop(void)` | 停止录音 |

录音参数固定：

```c
#define REC_SAMPLE_RATE  16000
#define REC_CHANNELS     2
#define REC_SAMPLE_BITS  16
```

录音线程名为 `rec_work`，栈 1024 字节，优先级 13。底层调用 `wavrecord -s <path> 16000 2 16`。

## 8. AgentConfig：运行时配置

头文件：`config/AgentConfig.h` 实现：`config/AgentConfig.c`

```c
typedef struct agent_runtime_config
{
    char api_key[AGENT_CFG_API_KEY_MAX];
    char model_name[AGENT_CFG_MODEL_MAX];
    char api_url[AGENT_CFG_API_URL_MAX];
    rt_bool_t is_configured;
} rt_align(RT_ALIGN_SIZE) agent_runtime_config_t;
```

| 函数 | 说明 |
|---|---|
| `agent_runtime_config_t *agent_config_get(void)` | 取静态配置指针 |
| `void agent_config_set(const char *api_key, const char *model_name, const char *api_url)` | 热更新配置，参数可为 `RT_NULL` |
| `const char *get_dynamic_agent_api_key(void)` | 优先返回运行时值，否则返回 `PKG_AGENT_API_KEY` |
| `const char *get_dynamic_agent_model_name(void)` | 同上，回退 `PKG_AGENT_MODEL_NAME` |
| `const char *get_dynamic_agent_api_url(void)` | 同上，回退 `PKG_AGENT_API_URL` |

三个字符串参数各自最多 127 字节。超过上限的参数会被忽略，其余参数照常更新，接口不返回错误。

## 9. Prompt：系统提示词

头文件：`include/prompt.h` 实现：`src/prompt.c`

| 函数 | 说明 |
|---|---|
| `char *get_system_prompt(void)` | 返回静态系统提示词，不要释放 |

当前内容是仓库示例，接入产品时应按场景修改。

## 10. MSH 命令

| 命令 | 来源 | 说明 |
|---|---|---|
| `main_loop_entry` | AgentLoop | 启动主循环 |
| `cleanup_agent_entry` | AgentLoop | 停止并清理全部资源 |
| `init_tools` | tool_func | 手动注册工具，通常由 `init_agent` 调用 |
| `agent_send_message <type> <content>` | Debug 通道 | 注入消息，类型 0–4 |
| `show_llm_config` | WebNet 通道 | 打印 API Key 是否设置、模型与地址 |
| `msh_agent_upload_download up <file_path>` | utils | 上传文件 |
| `msh_agent_upload_download down <file_id> <save_path>` | utils | 按文件 ID 下载 |
| `mqtt_tool connect\|disconnect\|start\|stop\|status\|list\|sub\|unsub\|pub\|recv\|history\|rule\|rules\|sim\|flush` | MQTT 工具 | MQTT 调试命令 |

`msh_agent_upload_download` 支持 `up` / `upload` 与 `down` / `download` 两组写法，也接受 `-h`、`--help`、`help`。

## 11. 已知差异

- `include/utils.h` 声明的是 `rt_err_t down_load(const char *path, const char *file_id)`，`src/utils.c` 实际定义的是 `agent_down_load(...)`。按声明调用会出现链接错误，当前应使用 `agent_down_load()`；也可以直接用 MSH 命令下载。
- `get_agent_file_op_mq_input()` 与 `get_agent_file_op_mq_output()` 已在实现中导出，但 `utils.h` 没有声明。需要调用时，先补声明或改用 MSH 命令。
- LVGL 通道的 ops 实现不在包内，由应用工程提供。只启用宏而不实现 `agent_lvgl_ops`，链接会失败。
- `cleanup_agent_entry` 不停止 MQTT 客户端。需要断开时先执行 `mqtt_tool stop` 或调用 `mqtt_tool_stop()`。
