# Agent API 文档

本文按模块列出公开接口。框架结构与数据流见 [framework.md](./framework.md)。

## 目录

1. [MessageHub：消息中心与消息数组](#1-messagehub消息中心与消息数组)
2. [Chat：LLM 通信](#2-chatllm-通信)
3. [Context：上下文管理器](#3-context上下文管理器)
4. [Tool：工具系统](#4-tool工具系统)
5. [Channels：交互通道](#5-channels交互通道)
6. [Utils：通用工具、多模态与语音](#6-utils通用工具多模态与语音)
7. [AgentAudio：音频驱动](#7-agentaudio音频驱动)
8. [AgentConfig：运行时配置](#8-agentconfig运行时配置)
9. [Prompt：系统提示词](#9-prompt系统提示词)
10. [MSH 命令](#10-msh-命令)
11. [已知差异](#11-已知差异)
12. [Skill：技能模块](#12-skill技能模块)
13. [AgentFsm：工具调用状态机](#13-agentfsm工具调用状态机)

设备框架工具的完整说明见 [tool_device.md](./tool_device.md)，验证方法见 [testing.md](./testing.md)。

## 1. MessageHub：消息中心与消息数组

头文件：`inc/core/MessageHub.h` 实现：`src/core/MessageHub.c`

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

头文件：`inc/core/chat.h` 实现：`src/core/chat.c`

`chat.c` 只处理 HTTP 与 SSE 行拆分，请求体、鉴权头和事件解析都交给协议模块，位置在 `src/utils/protocol/`，统一入口 `inc/utils/protocol.h`。当前协议决定报文格式，切换接口见 2.3。

请求地址由「服务根地址 + 协议路径」拼出来。菜单里填两个 base URL，路径框架自己接：

| 协议 | Kconfig 里的 base URL | 框架补的路径 | 实际请求地址 |
|---|---|---|---|
| OpenAI | `PKG_AGENT_API_BASE_URL_OPENAI`，默认 `https://api.deepseek.com` | `/chat/completions` | `https://api.deepseek.com/chat/completions` |
| Anthropic | `PKG_AGENT_API_BASE_URL_ANTHROPIC`，默认 `https://api.deepseek.com/anthropic` | `/v1/messages` | `https://api.deepseek.com/anthropic/v1/messages` |

`get_dynamic_agent_request_url()` 负责拼接，base URL 结尾的斜杠会先去掉，避免拼出 `//v1/messages`。运行时没有覆盖过 base URL 时，切换协议会跟着换用另一份 base URL；一旦通过 `agent_config_set()` 覆盖过，就固定用覆盖值，切协议不再自动换。

### 2.1 类型

```c
typedef struct ChatResponse
{
    char *reasoning;
    char *context;
    cJSON *tool_call;
    rt_bool_t complete;
} ChatResponse, *ChatResponse_t;
```

- `reasoning`：思考文本。
- `context`：回答正文。
- `tool_call`：工具调用数组，元素为 OpenAI 格式的 tool call 对象。
- `complete`：仅表示自然文本完整结束。必须收到 `[DONE]` 和 `finish_reason: "stop"`，正文没有丢弃或截断，且没有工具调用。它不表示工具调用完成或执行成功。

### 2.2 接口

#### `ChatResponse_t chat(cJSON *messages, cJSON *tools, int max_tokens, void (*on_reasoning)(const char *text), void (*on_tool_call)(const char *text), void (*on_context)(const char *text))`

发送请求并流式读取 SSE 响应。

- `messages`：消息数组，函数内部复制，不取走所有权。
- `tools`：工具定义数组，可传 `RT_NULL`，此时请求体不包含 `tools`。
- `max_tokens`：最大 token 数，由调用方指定。普通对话传 32 × 1024，上下文摘要请求传 512。
- 回调都在流式解析过程中调用，可传 `RT_NULL`。
- `on_tool_call` 在响应结束后收到完整的工具调用 JSON。
- 返回 `ChatResponse_t` 时，必须用 `chat_response_free()` 释放。请求建立或资源分配失败返回 `RT_NULL`；流中断等情况仍可能返回部分响应。
- 网络超时为 30 s。

解析规则：

- 处理以 `data:` 开头的行，冒号后允许一个空格，支持 LF 和 CRLF 换行。OpenAI 遇到 `[DONE]` 结束，Anthropic 遇到 `message_stop` 结束。
- OpenAI 的思考文本优先取 `delta.reasoning_content`，没有时取 `delta.reasoning`；Anthropic 取 `delta.thinking`。
- OpenAI 的正文取 `delta.content`；Anthropic 取 `delta.text_delta.text`。
- 工具调用按 `index` 合并，`id` 与 `name` 取第一个非空值，`arguments` 逐片拼接。Anthropic 的 `input_json_delta.partial_json` 走同一条拼接路径。
- 缺失 `id` 的调用用 `call_<index>` 补齐，空参数改为 `{}`，没有函数名的调用丢弃。
- JSON 或响应字段格式错误、SSE 行过长被丢弃、正文超限截断或流未完整结束时，`complete` 为 `RT_FALSE`。出现工具调用时也为 `RT_FALSE`，即使该调用最后被丢弃。

普通对话仍沿用部分响应的处理方式，目前只有上下文摘要调用方检查 `complete`。拿到非空响应指针不代表正文完整。

#### `void chat_response_free(ChatResponse_t resp)`

释放思考文本、正文、工具调用数组与结构体。`resp` 为 `RT_NULL` 时直接返回。

### 2.3 协议切换

头文件 `inc/utils/protocol.h`，实现在 `src/utils/protocol/`。

| 函数 | 说明 |
|---|---|
| `const agent_protocol_ops_t *agent_protocol_get_ops(void)` | 当前协议的操作表。表里有 `build_request`、`add_headers`、`parse_event`、`format_content` 四个函数指针 |
| `agent_api_protocol_t agent_protocol_current(void)` | 当前协议，取自运行时配置 |
| `void agent_protocol_switch(agent_api_protocol_t protocol)` | 切换协议，同时改写运行时配置。下次发请求按新协议拼包 |

`agent_api_protocol_t` 有两个值：`AGENT_API_PROTOCOL_OPENAI` 与 `AGENT_API_PROTOCOL_ANTHROPIC`。

`agent_protocol_switch()` 改报文格式，也换请求地址：base URL 没被运行时覆盖过时，会跟着切到另一份 Kconfig base URL。已经用 `agent_config_set()` 覆盖过 base URL 的话，地址就固定在覆盖值上，切协议只换报文格式。

消息在上下文里一律存规范格式，也就是 OpenAI 风格。协议模块负责转成自己的报文，解析时转回规范格式，因此两种协议下 `chat()` 的入参、返回结构和工具调用 JSON 完全一致，切换协议不需要改调用方。

Anthropic 报文与规范格式的对应关系见 [framework.md](./framework.md) 3.8。

## 3. Context：上下文管理器

头文件：`inc/core/context.h` 实现：`src/core/context.c`

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
    rt_bool_t has_summary;
    rt_err_t (*build_system_prompt)(struct Context *self);
    rt_err_t (*append_user_message)(struct Context *self, Messages_t messages);
    rt_err_t (*append_assistant_message)(struct Context *self, ChatResponse_t resp);
    rt_err_t (*append_tool_message)(struct Context *self, const char *tool_call_id, Messages_t messages);
    rt_err_t (*clear_message)(struct Context *self);
    rt_err_t (*compress_context)(struct Context *self, int threshold);
} Context, *Context_t;
```

`has_summary` 表示索引 1 处是否有滚动摘要。创建时为 `RT_FALSE`，压缩成功后为 `RT_TRUE`，清空成功后复位。

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
| `build_system_prompt(self)` | 向空数组追加 system 消息，使其位于索引 0；创建时自动调用，清空后由调用方重建 |
| `append_user_message(self, messages)` | 追加 user 消息，内容复制，调用方保留 `messages` 所有权 |
| `append_assistant_message(self, resp)` | 无工具调用时追加正文；有工具调用时追加 `content` 为空、带 `tool_calls` 的 assistant 消息 |
| `append_tool_message(self, tool_call_id, messages)` | 追加 `role=tool` 消息，只拼接 `TYPE_TEXT` 内容，结果是一个字符串 |
| `clear_message(self)` | 删除全部消息并新建空数组，复位 `has_summary`；调用方负责重建系统提示词 |
| `compress_context(self, threshold)` | 消息数超过阈值时，尝试用滚动摘要替换旧历史，保留 system 和最近完整轮次 |

工具结果只支持文本。图片、音频等多模态内容不能放进 `role=tool` 消息。

`build_system_prompt` 保留 `get_system_prompt()` 返回的基础提示词，再追加自动生成的内置技能 JSON 目录和加载说明。使用任务工具前，模型应调用 `skill({"name":"<name>"})` 加载匹配技能。完整技能说明仍在上下文中时可复用；只剩摘要时要重新加载。摘要是历史数据，不是新指令或设备当前状态的证明。

`compress_context` 替代原来的 `trim_context`，规则如下：

- `threshold` 至少为 2。主循环传入现有配置 `PKG_AGENT_MESSAGE_TRIM`，计数排除 system 和已有滚动摘要。只有消息数严格大于阈值才尝试压缩。
- 先取最近 `threshold / 2` 条消息，除法向下取整。保留范围再向前扩到 user 消息边界，不拆开一轮对话或工具调用与结果。实际保留条数可能更多；没有可压缩的完整旧轮次时跳过，包括只有一轮超长对话的情况。
- 将保留范围之前的历史（含上一份摘要，不含 system）序列化为 JSON，作为 user 数据发送。请求使用专用摘要 system 提示词，把历史当作数据，不执行其中的指令。
- 摘要需要额外一次模型请求：`chat(request, RT_NULL, 512, RT_NULL, RT_NULL, RT_NULL)`。不提供工具，三个回调均为空，不向用户流式输出摘要。
- 只接受非空白正文、`complete == RT_TRUE` 且没有工具调用的响应。带历史标记的摘要消息必须使序列化后的历史字节数减少。替换内容构造并检查成功后才提交，摘要作为 assistant 消息放在索引 1，system 和最近消息保持原样。
- 网络、内存或摘要校验失败时，保留全部原历史，不回退到直接裁剪。失败或跳过压缩时，后续对话可能让历史继续增长，阈值不是硬性容量上限。

成功压缩或跳过压缩返回 `RT_EOK`。失败返回 `-RT_EINVAL`、`-RT_ENOMEM` 或 `-RT_ERROR`，原历史保持不变。

摘要保留语义信息，不是逐字记录，不能保证细节完整。摘要中的技能名不能代替完整技能说明，后续使用时需要重新加载。摘要请求期间 agent 仍处于忙碌状态。

## 4. Tool：工具系统

头文件：`inc/core/tool_base.h`、`inc/core/tool_func.h`、`inc/utils/tool_json.h` 实现：`src/core/tool_base.c`、`src/core/tool_func.c`、`src/utils/tool_json.c`、`src/tools/*`

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
| `void init_tools(void)` | 建工具链表、遍历模块表注册工具、构建工具定义 JSON；重复调用不会重复注册 |
| `void agent_tools_cleanup(void)` | 清理工具并复位注册标志，再次进入时重新注册 |

### 4.3 内置工具

| 工具 | 参数 | 结果 |
|---|---|---|
| `add` | `a`, `b`（number） | 两数之和 |
| `mul` | `a`, `b`（number） | 两数之积 |
| `compare` | `a`, `b`（number） | 大小比较文本 |

三个工具各占一个 `.c`，`add`、`mul`、`compare` 三张描述符表在文件末尾各自登记，`init_tools()` 不再逐项拼 JSON。写法见 [tool_guide.md](./tool_guide.md) 第 3 节。

### 4.4 设备框架工具

头文件：`src/tools/tool_device/tool_device_internal.h`（模块内部约定），注册接口在 `inc/utils/tool_json.h`
实现：`src/tools/tool_device/`（通用层 `tool_device_core.c`，一类设备一个模块文件）
使用说明：[tool_device.md](./tool_device.md)

通用层（`PKG_AGENT_TOOL_DEVICE_CORE` 且 `RT_USING_DEVICE`）注册 7 个工具。它们统一走 `rt_device_*` 框架接口，模型不需要了解具体驱动：

| 工具 | 参数 | 说明 |
|---|---|---|
| `device_list` | `class?`（string） | 列出已注册设备，可按类名或类号过滤，只读 |
| `device_info` | `name`（string，必填） | 查询类、标志、引用计数、驱动支持的操作，只读 |
| `device_open` | `name`（必填），`mode?`（rdonly / wronly / rdwr），`flags?`（数组） | 打开设备，引用计数加一，首次打开触发初始化 |
| `device_close` | `name`（必填） | 关闭一次；拒绝关闭控制台设备 |
| `device_read` | `name`（必填），`count?`，`pos?` | 读数据并返回 hex 与可打印文本；拒绝读控制台设备 |
| `device_write` | `name`、`data`（必填），`encoding?`（text / hex），`pos?` | 写数据，块设备必须整块写；需要用户授权 |
| `device_control` | `name`、`cmd`（必填），`arg?`（number） | 执行框架控制命令，命令名见技能 `device-commands` |

类模块一共 22 个工具，由 Kconfig 逐项决定编不编，开关是 `PKG_AGENT_TOOL_DEVICE_<模块>`：

| 开关 | 内核依赖 | 工具 |
|---|---|---|
| `..._CORE` | 无 | `device_list` 等 7 个通用工具 |
| `..._PIN` | `RT_USING_PIN` | `pin_mode`、`pin_write`、`pin_read` |
| `..._PWM` | `RT_USING_PWM` | `pwm_set`、`pwm_get`、`pwm_enable`、`pwm_disable` |
| `..._ADC` | `RT_USING_ADC`，不能开 ADC V2 | `adc_read` |
| `..._DAC` | `RT_USING_DAC` | `dac_write`、`dac_enable`、`dac_disable` |
| `..._I2C` | `RT_USING_I2C` | `i2c_write`、`i2c_read`、`i2c_writeread` |
| `..._SPI` | `RT_USING_SPI` | `spi_configure`、`spi_transfer` |
| `..._SENSOR` | `RT_USING_SENSOR`，不能开 v2 | `sensor_read`、`sensor_info`、`sensor_control` |
| `..._LED` | `RT_USING_LED` | `led_set`、`led_get`、`led_brightness` |

每个模块文件末尾用 `AGENT_TOOL_MODULE_EXPORT(标签, 工具表)` 登记自己，宏内部走 `INIT_COMPONENT_EXPORT`。`init_tools()` 只调一次 `agent_tool_register_modules()`，把模块表里全部工具挂上工具链表。设备模块与运算、技能、MQTT 工具共用这套注册机制。

约定：

- 字符设备忽略 `pos`，`count` 按字节计；块设备的 `pos` 与 `count` 按块计，`device_read`、`device_write` 内部先查 `BLK_GETGEOME` 拿块大小。
- `device_control` 封装了 `BLK_GETGEOME`、`RTC_GET_TIME`、`RTC_SET_TIME`、`WDT_GET_TIMEOUT`、`WDT_SET_TIMEOUT`、`WDT_GET_TIMELEFT` 的参数。其余命令要么无参调用，要么因参数是驱动私有结构而拒绝。
- 读、写、控制都要求设备已打开。读到 0 字节会写明原因，写入返回实际接受的字节数。
- 控制台设备禁止 `device_read` 与 `device_close`：前者会吃掉 shell 输入，后者会中断 shell。
- 类模块里 `sensor_read` 与 3 个 LED 工具要求先 `device_open`；其余走各自类的专用接口，不需要打开。

模块表在运行时可查，四个接口返回本次编译进来了哪些模块和工具（声明见 6.2）：

| 接口 | 返回 |
|---|---|
| `agent_tool_module_count()` | 已登记的模块数 |
| `agent_tool_module_name(index)` | 第 `index` 个模块的标签，例如 `"pin"`；越界返回 `RT_NULL` |
| `agent_tool_module_tools(index)` | 该模块的工具数；越界返回 0 |
| `agent_tool_module_tool_name(module, tool)` | 该模块第 `tool` 个工具的名字；越界返回 `RT_NULL` |

两个配套技能由 `skill` 工具加载：`device` 说明工作流，`device-commands` 给出命令目录。工具说明见 [tool_device.md](./tool_device.md)。

### 4.5 MQTT 工具

头文件：`inc/tools/tool_mqtt/tool_mqtt.h`
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

### 4.6 MQTT 应用层接口

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

### 4.7 Agent 运行时访问器

头文件：`inc/core/AgentRuntime.h`

| 函数 | 说明 |
|---|---|
| `MessageHub_t agent_get_message_hub(void)` | 当前消息中心，agent 未运行时返回 `RT_NULL` |
| `rt_bool_t agent_is_running(void)` | 主循环是否正在运行 |
| `rt_bool_t agent_is_busy(void)` | 是否正在处理一次用户请求，含上下文摘要阶段，注入方可据此错峰 |

### 4.8 文本注入

头文件：`inc/utils.h`

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

### 4.9 MCP 客户端接口

头文件：`inc/tools/tool_mcp/mcp_client.h`，仅在 `PKG_AGENT_TOOL_MCP_ENABLE` 打开时编译。

| 函数 | 说明 |
|---|---|
| `rt_err_t mcp_client_connect(void)` | 连接服务端并完成 `initialize`、`notifications/initialized`、`tools/list` 握手，已连接时直接返回 `RT_EOK` |
| `cJSON *mcp_client_tools(void)` | 远端工具表，`tools/list` 返回的数组，由客户端持有，未连接返回 `RT_NULL` |
| `rt_err_t mcp_client_call(const char *name, cJSON *arguments, char **out_text)` | 调用一个远端工具，`name` 用远端原名，不带 `mcp_` 前缀 |
| `void mcp_client_disconnect(void)` | 清掉会话 ID 和远端工具表，不发 HTTP 请求 |

`mcp_client_call` 的 `arguments` 只读，函数内部自行拷贝。`out_text` 是 `rt_malloc` 出来的
文本，调用方负责 `rt_free`。服务端报错或 `isError` 为真时返回 `-RT_ERROR`，`out_text` 里
是失败原因。模块整体说明见 [tool_mcp.md](./tool_mcp.md)。

## 5. Channels：交互通道

头文件：`inc/channels/*` 实现：`src/channels/*`

### 5.1 通道接口

```c
typedef struct AgentChannelOps
{
    int  (*init)(MessageHub_t hub, Context_t ctx);
    void (*reset)(void);
} AgentChannelOps;
```

`AgentLoop` 通过 `AGENT_CHANNEL_OPS` 选择通道，选择点在 `inc/channels/AgentChannels.h`。优先级为 LVGL → CLI → WebNet → Debug，未配置时返回 `RT_NULL`。

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
实现：`src/channels/AgentDebugChannel.c`

| 函数 | 说明 |
|---|---|
| `int agent_debug_channel(MessageHub_t hub, Context_t ctx)` | 绑定 hub 与 ctx |
| `void agent_debug_reset(void)` | 置空缓存指针，幂等 |
| `extern AgentChannelOps agent_debug_ops` | `{ agent_debug_channel, agent_debug_reset }` |

MSH 命令：

```
agent_send_message <type> <content>
```

`type` 取 0 到 4，依次是 text、audio、image、video、null，0 表示文本。命令把消息投进输入邮箱，然后等输出，最多等 30 秒。超时、agent 没运行或参数个数不足时返回 -1。

本项目在 `rtconfig.h` 里定义了 `PKG_AGENT_DEBUG_CHANNEL`，当前选中的是 Debug 通道。用它做真机验证的方法见 [testing.md](./testing.md)。

### 5.5 LVGL 通道

宏：`PKG_AGENT_LVGL_CHANNEL`
头文件：`inc/channels/AgentLvglChannel.h`

| 函数 | 说明 |
|---|---|
| `int agent_lvgl_channel(MessageHub_t hub, Context_t ctx)` | 保存 hub，不使用 ctx |
| `void agent_lvgl_channel_reset(void)` | 清空 hub，幂等 |
| `MessageHub_t agent_lvgl_channel_hub(void)` | 取当前 hub，未初始化时返回 `RT_NULL` |
| `extern AgentChannelOps agent_lvgl_ops` | `{ agent_lvgl_channel, agent_lvgl_channel_reset }` |

包内只有接口，`agent_lvgl_ops` 由应用实现。本仓库示例在 `applications/lvgl/demo/lv_home_agent.c`：用 `message_hub_get_timeout()` 轮询输出，通过 MSH 命令启动 agent。本项目暂时没开这个通道。

## 6. Utils：通用工具、多模态与语音

头文件：`inc/utils.h`，它只转发到 `inc/utils/utils_internal.h`，由后者按启用的宏导入模块头。声明在 `inc/utils/`、实现在 `src/utils/`，两边同名：`message`、`format`、`tool_json`、`file_op`、`stt`、`tts`。

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

### 6.2 工具 JSON 构建与模块注册

头文件 `inc/utils/tool_json.h`，实现 `src/utils/tool_json.c`。

构造函数：

| 函数 | 说明 |
|---|---|
| `cJSON *create_tool_item(const char *func_name, const char *desc, cJSON *params_obj)` | 生成工具定义 |
| `cJSON *create_param_obj(cJSON *props_obj, cJSON *required_arr)` | 生成 `{type:"object", properties, required}` |
| `void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc)` | 添加一个属性 |
| `void tool_desc(cJSON *func, const char *text)` | 写入功能说明主体 |
| `void tool_desc_more(cJSON *func, const char *text)` | 追加一句说明，前面补一个空格 |
| `void tool_desc_read_only(cJSON *func)` | 追加 `Read-only.` |
| `void tool_desc_state_changing(cJSON *func)` | 追加 `State-changing: call only after the user authorized this peripheral.` |

`tool_desc` 系列接收的是工具项里的 `function` 子对象。注册时 description 先以空串占位，再由 `_desc` 回调覆盖。覆盖走 `cJSON_ReplaceItemInObjectCaseSensitive`，键顺序保持 `name`、`description`、`parameters`。

模块表。设备工具、运算工具、技能与 MQTT 工具都用这一套：

| 名字 | 说明 |
|---|---|
| `struct agent_tool_def` | 一个工具的静态描述：`name`、`describe` 回调、`params` 构造函数、`execute` 执行函数 |
| `struct agent_tool_module_def` | 一个模块：模块标签、工具表指针、工具数 |
| `AGENT_TOOL_MODULE_EXPORT(label, tools)` | 生成模块描述符，并用 `INIT_COMPONENT_EXPORT` 在内核组件初始化阶段登记 |
| `rt_err_t agent_tool_module_add(const struct agent_tool_module_def *module)` | 登记一个模块。描述符不合法返回 `-RT_EINVAL`，表满返回 `-RT_EFULL`，重复登记返回 `RT_EOK` |
| `void agent_tool_register_modules(void)` | 遍历模块表，依次调用每个工具的 `params` 构造函数、`create_param_obj`、`create_tool_item`、`describe` 回调、`append_tool`；由 `init_tools()` 调用 |
| `int agent_tool_module_count(void)` | 已登记的模块数 |
| `const char *agent_tool_module_name(int index)` | 第 `index` 个模块的标签，例如 `"pin"`；越界返回 `RT_NULL` |
| `int agent_tool_module_tools(int index)` | 该模块的工具数；越界返回 0 |
| `const char *agent_tool_module_tool_name(int module, int tool)` | 该模块第 `tool` 个工具的名字；越界返回 `RT_NULL` |

模块表容量 24 项，登记表指针去重。`describe` 和 `params` 构造函数分别形如 `void (*)(cJSON *func)` 与 `void (*)(cJSON *props, cJSON *required)`。`params` 填 `RT_NULL` 时注册结果是一个空 `properties` 加空 `required`；`describe` 填 `RT_NULL` 时 description 留空，日志打一条 `Tool '<name>' has no description`。

### 6.3 本地语音转写

仅在 `PKG_AGENT_STT_ENABLE` 时编译。`agent_stt_transcribe(const char *path)` 定义在 `src/utils/stt.c`，声明在 `inc/utils/stt.h`，由 `to_content()` 处理音频时调用。

- 向 `PKG_AGENT_STT_URL` 发送 multipart POST。
- 连接建立后把收发超时放宽到 120 s，首次加载 whisper 模型可能耗时较久。
- 解析 `{"code":0,"data":{"text":"..."}}`。
- 响应缓冲为静态 1024 字节。
- 失败返回 `RT_NULL`，由调用方生成提示文本。

文件服务器端的 `/stt` 接口见 [resource/file_server/README.md](../resource/file_server/README.md)。

### 6.4 文本转语音

仅在 `PKG_AGENT_TTS_ENABLE` 时编译，实现文件 `src/utils/tts.c`。板端到喇叭的过程见 [TTS 运行时链路](diagrams/tts-runtime-flow.png)。

对外函数声明在 `inc/utils/tts.h`，只在 `PKG_AGENT_TTS_AUTO_PLAY` 打开时存在，该开关依赖 `PKG_AGENT_DRIVER_AUDIO`，由 `AgentLoop.c` 调用：

```c
void agent_tts_speak_init(void);        /* init_agent() 里创建队列和线程 */
void agent_tts_speak(const char *text); /* 把回复文本入队，不等合成 */
```

`agent_tts_speak()` 把文本拷到堆上，只让指针入队。队列满或者内存不够时打印一条警告，丢掉这条回复。`agent_tts_synth(const char *text, const char *save_path, rt_off_t *out_len)` 是 `src/utils/tts.c` 里的静态函数，只服务 `msh_agent_tts`，不进头文件。

请求与响应：

| 项 | 取值 |
|---|---|
| 方法 | `POST PKG_AGENT_TTS_URL`，`Content-Type: application/json` |
| 整条合成 | `{"text":"…","sample_rate":16000,"channels":1}`，响应体是一条完整 WAV |
| 流式合成 | 上面再加 `"stream":true,"block_seconds":1.5`，响应体是 `[4 字节小端长度][完整 WAV]` 的帧序列 |
| 声道数 | 1。`libraries/HAL_Drivers/drv_i2s.c` 的 `convert_mono_to_stereo()` 负责复制成左右两个 PCM 槽，喂双声道会变成两倍时长、低八度 |
| 超时 | 连接建立后放宽到 5 min，取 `AGENT_TTS_TIMEOUT_MS` |
| 结束标志 | 流式以连接关闭结束，没有单独的结束帧，也没有状态帧。服务端合成中途失败也走这条收尾，板侧只能按已收到的帧数判断说到哪儿 |

返回值：`agent_tts_synth()` 成功返回 `RT_EOK`，落地字节数写进 `out_len`，该参数可以为 `NULL`。文本或路径为空、`PKG_AGENT_TTS_URL` 为空返回 `-RT_EINVAL`。连接、收流、写盘任一段出错返回负值，串口打印 HTTP 状态码或者断点位置。`Content-Length` 已知而实际收到的字节更少算失败，不留半条 WAV。

模块常量集中在 `inc/utils/tts.h`，用 `AGENT_` 前缀：`AGENT_TTS_IO_BUF_SZ` 8192、`AGENT_TTS_SLOT_PATH_SZ` 64、`AGENT_TTS_BLOCK_SECONDS` 1.5、`AGENT_TTS_MAX_BLOCKS` 64、`AGENT_TTS_SPEAK_QUEUE` 8、`AGENT_TTS_SPEAK_STACK` 8192、`AGENT_TTS_PLAY_WAIT_MS` 60000。能在 menuconfig 里改的参数保留 `PKG_AGENT_` 前缀，由 `rtconfig.h` 提供。

文件服务器端的 `/tts` 接口见 [resource/file_server/README.md](../resource/file_server/README.md)。

### 6.5 多模态文件操作

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

### 6.6 webclient 超时设置

头文件：`inc/utils/webclient_util.h` 实现：`src/utils/webclient_util.c`

| 函数 | 说明 |
|---|---|
| `void agent_webclient_set_timeout(struct webclient_session *session, int millisecond)` | 把会话的收发超时设成指定毫秒数。socket 没连上时直接返回，什么都不做 |

包内五条链路都用它：聊天、MCP 客户端、TTS、STT、文件上传下载。调用点都在连接成功
之后，因为 webclient v2.2.0 的超时只能设在已经连上的 socket 上。细节见
[framework.md 的 3.8 节](./framework.md)。

`utils.h` 的声明与实际符号存在差异，见第 11 节。

## 7. AgentAudio：音频驱动

头文件：`inc/driver/audio/AgentAudio.h` 实现：`src/driver/audio/AgentAudio.c`

启用宏：`PKG_AGENT_DRIVER_AUDIO`

| 函数 | 说明 |
|---|---|
| `int agent_driver_audio_play(const char *path)` | 播放 WAV，`wavplayer` 起播就返回，调用不阻塞 |
| `int agent_driver_audio_play_wait(rt_int32_t timeout_ms)` | 每 100 ms 轮询一次播放状态，播完返回 `RT_EOK`，超时返回 `-RT_ETIMEOUT`。TTS 覆盖文件前用它等上一句放完 |
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

头文件：`inc/core/AgentConfig.h`，实现：`src/core/AgentConfig.c`

```c
typedef struct agent_runtime_config
{
    char api_key[AGENT_CFG_API_KEY_MAX];
    char model_name[AGENT_CFG_MODEL_MAX];
    char base_url[AGENT_CFG_BASE_URL_MAX];
    agent_api_protocol_t protocol;
    rt_bool_t is_configured;
} rt_align(RT_ALIGN_SIZE) agent_runtime_config_t;
```

| 函数 | 说明 |
|---|---|
| `agent_runtime_config_t *agent_config_get(void)` | 取静态配置指针 |
| `void agent_config_set(const char *api_key, const char *model_name, const char *base_url)` | 热更新配置，参数可为 `RT_NULL` |
| `const char *get_dynamic_agent_api_key(void)` | 优先返回运行时值，否则返回 `PKG_AGENT_API_KEY` |
| `const char *get_dynamic_agent_model_name(void)` | 同上，回退 `PKG_AGENT_MODEL_NAME` |
| `const char *get_dynamic_agent_base_url(void)` | 同上，回退当前协议对应的 `PKG_AGENT_API_BASE_URL_*` |
| `const char *get_dynamic_agent_request_url(void)` | base URL 加协议路径，见第 2 节的拼接表。返回内部静态缓冲，同一时刻只供一个请求线程用 |
| `agent_api_protocol_t get_dynamic_agent_api_protocol(void)` | 当前协议，默认取 `PKG_AGENT_API_PROTOCOL_SEL` 选中的那个 |
| `void agent_config_set_protocol(agent_api_protocol_t protocol)` | 切换协议 |

三个字符串参数各自最多 127 字节。超过上限的参数会被忽略，其余参数照常更新，接口不返回错误。

`agent_config_set()` 不改协议，协议由 `agent_config_set_protocol()` 或 `agent_protocol_switch()` 单独设置。

## 9. Prompt：系统提示词

头文件：`inc/core/prompt.h` 实现：`src/core/prompt.c`

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
| `agent_send_message <type> <content>` | Debug 通道 | 注入消息，类型 0–4，等输出最多 30 s |
| `show_llm_config` | WebNet 通道 | 打印 API Key 是否设置、模型与地址 |
| `msh_agent_upload_download up <file_path>` | utils | 上传文件 |
| `msh_agent_upload_download down <file_id> <save_path>` | utils | 按文件 ID 下载 |
| `msh_agent_tts [-f] [-o <path>] <text>` | utils | 合成语音落盘并播放 |
| `agent_protocol [openai\|anthropic]` | utils | 查看或切换 LLM 协议 |
| `mqtt_tool connect\|disconnect\|start\|stop\|status\|list\|sub\|unsub\|pub\|recv\|history\|rule\|rules\|sim\|flush` | MQTT 工具 | MQTT 调试命令 |

`msh_agent_upload_download` 支持 `up` / `upload` 与 `down` / `download` 两组写法，也接受 `-h`、`--help`、`help`。

`msh_agent_tts` 需要 `PKG_AGENT_TTS_ENABLE`。`-f` 只合成不播放，`-o` 改落盘路径（默认取 `PKG_AGENT_TTS_SAVE_FILE`），其余参数用空格拼成待合成文本。播放依赖 `PKG_AGENT_DRIVER_AUDIO`，没打开时只落盘并给出提示。

## 11. 已知差异

- LVGL 通道的 ops 实现不在包内，由应用工程提供。只启用宏而不实现 `agent_lvgl_ops`，链接会失败。
- `cleanup_agent_entry` 不停止 MQTT 客户端。需要断开时先执行 `mqtt_tool stop` 或调用 `mqtt_tool_stop()`。

## 12. Skill：技能模块

头文件：`inc/core/skill.h` 实现：`src/core/skill.c`
工具：`src/tools/tool_skill.c`，文件末尾用 `AGENT_TOOL_MODULE_EXPORT(skill, skill_tools)` 登记，工具名是 `skill`

### 12.1 类型

```c
struct agent_skill
{
    const char *name;
    const char *content;
    size_t size; /**< UTF-8 字节数，不含结尾的 NUL。 */
};
```

### 12.2 接口

| 函数 | 说明 |
|---|---|
| `size_t agent_skill_count(void)` | 内嵌技能个数 |
| `const struct agent_skill *agent_skill_get(size_t index)` | 按下标取技能。越界返回 `RT_NULL` |
| `const struct agent_skill *agent_skill_find(const char *name)` | 按名字取技能。名字为 `RT_NULL` 或不存在时返回 `RT_NULL` |
| `const char *agent_skill_list(void)` | 返回技能目录的 JSON 文本，不释放 |

目录是 JSON 数组，每项含 `name`、`title`、`size`。`title` 取技能 Markdown 里第一个 `# ` 标题，没有标题就用技能名。

### 12.3 技能工具 `skill`

| 参数 | 类型 | 说明 |
|---|---|---|
| `name` | string | 可选。技能名。省略时返回目录 |

不带 `name` 时返回 `agent_skill_list()` 的内容。带 `name` 时返回该技能的完整 Markdown，技能不存在返回 `-RT_EINVAL`，文本为 `Skill not found. Call skill without name to list available skills.`。

### 12.4 内嵌过程

技能文件放在 `skills/` 目录，文件名去掉 `.md` 就是技能名。`skills/SConscript` 调用 `skills/embed_skills.py`，在构建时生成 `skill_data.c`，里面是 `agent_skill_table[]`、`agent_skill_table_count` 和 `agent_skill_catalog` 三份数据。技能目录由 menuconfig 选项 `PKG_AGENT_SKILL_DIR` 决定，默认 `skills`；SCons 参数 `agent_skill_dir` 可以只改这一次构建。

目录会注入系统提示词，见第 3 节 `build_system_prompt`。上下文被压缩后摘要里只留技能名，要继续用某个技能，先重新调用 `skill` 加载。

## 13. AgentFsm：工具调用状态机

头文件：`inc/core/AgentFsm.h` 实现：`src/core/AgentFsm.c`

这个模块只被 `src/core/AgentLoop.c` 调用。状态机不碰邮箱和 TTS，输出由 `AgentFsmOps` 注入。节点与边的流转见 [framework.md](./framework.md) 3.1。

### 13.1 宏

| 宏 | 值 | 说明 |
|---|---|---|
| `AGENT_LOOP_MAX` | `PKG_AGENT_TOOL_LOOP_MAX`，默认 6 | 一次请求最多跑几轮工具循环，取值范围 1～32 |
| `AGENT_CHAT_RETRY_MAX` | 3 | 一次 LLM 请求最多重试几次 |
| `AGENT_CHAT_MAX_TOKENS` | 32 × 1024 | 单次请求的 token 上限 |

### 13.2 类型

```c
typedef enum AgentFsmState
{
    AGENT_FSM_INIT = 0,     /* 初始化：清轨迹与护栏计数 */
    AGENT_FSM_THINK,        /* LLM 决策器：请求模型 */
    AGENT_FSM_VALIDATE,     /* 校验动作格式与参数 */
    AGENT_FSM_ACT,          /* 工具路由器：执行工具 */
    AGENT_FSM_OBSERVE,      /* 观察记录器：结果写回轨迹 */
    AGENT_FSM_GUARD,        /* 护栏：轮次上限与重复动作 */
    AGENT_FSM_FINISH,       /* 收尾：输出最终回答 */
    AGENT_FSM_DONE,         /* 结束 */
    AGENT_FSM_STATE_MAX
} AgentFsmState_t;

typedef enum AgentFsmAction
{
    AGENT_FSM_ACTION_NONE = 0,
    AGENT_FSM_ACTION_ERROR,      /* 参数或工具非法，记一条错误观察 */
    AGENT_FSM_ACTION_DUPLICATE,  /* 重复调用，记一条「结果未变」观察 */
    AGENT_FSM_ACTION_EXECUTE     /* 正常执行 */
} AgentFsmAction_t;

typedef struct AgentFsmOps
{
    void (*emit_answer)(Context_t context, ChatResponse_t resp, void (*on_context)(const char *text));
    void (*notify)(const char *text, void (*on_context)(const char *text));
} AgentFsmOps, *AgentFsmOps_t;
```

`AgentFsm_t` 是全局状态，字段分四组：依赖（`context`、`tools`、`ops`、三个流式回调）、轮次与终止（`loop_max`、`chat_retry_max`、`loop_cnt`、`attempt`、`dup_rounds`、`answered`、`resp`）、工具调用进度（`tool_count`、`tool_index`、`dup_hits`、`over_hits`）、当前动作（`action_kind`、`action_id`、`action_name`、`action_error`、`action_error_buf`、`action_args`、`action_tool`、`action_messages`）。

### 13.3 接口

| 函数 | 说明 |
|---|---|
| `void agent_fsm_run(AgentFsm_t *fsm)` | 从 `INIT` 开始跑完状态机，直到 `DONE` |

调用前要先填好 `AgentFsm_t` 的依赖字段。`resp` 由状态机自己管理：每轮请求后由 `GUARD` 释放，收尾时由 `FINISH` 释放，调用方不用管。

每次状态迁移都会打一行日志 `I/Agent.Fsm: [fsm] <当前节点> -> <下一个节点>`，便于在串口上回溯一轮对话的流转。
