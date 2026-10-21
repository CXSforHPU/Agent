# 工具开发指南

本文说明如何给 Agent 添加工具、注册工具，以及工具执行函数的约定。框架结构见 [framework.md](./framework.md)，接口见 [api.md](./api.md)。

## 1. 调用流程

工具调用由模型发起，框架负责执行和回填：

```text
LLM 返回 tool_calls（函数名 + 参数 JSON）
        ↓
agent_loop 按 function.name 查找工具节点
        ↓
校验参数 JSON，调用 tool_xxx(args, node)
        ↓
工具把结果写入 node->ret.messages
        ↓
结果以 role=tool 消息写入上下文
        ↓
进入下一轮 chat()
```

几条硬性规则：

- 工具函数签名固定为 `void tool_xxx(cJSON *args, AgentToolNode_t node)`。
- 工具结果必须是文本。`role=tool` 消息只接受字符串内容。
- 每个 `tool_call_id` 都必须有一条对应的 tool 消息。工具不存在、参数错误、执行失败时，框架也会补一条错误结果。
- 工具结果为空时，模型容易反复调用同一个工具。返回内容至少包含结果、失败原因或下一步建议。

## 2. 工具定义

每个工具以 OpenAI function-calling 格式发给模型：

```json
{
  "type": "function",
  "function": {
    "name": "add",
    "description": "Calculate the sum of two numbers",
    "parameters": {
      "type": "object",
      "properties": {
        "a": { "type": "number", "description": "First number" },
        "b": { "type": "number", "description": "Second number" }
      },
      "required": ["a", "b"]
    }
  }
}
```

框架提供三个构造函数（`include/utils.h`）：

| 函数 | 作用 |
|---|---|
| `create_property(props_obj, name, type, desc)` | 添加属性 `{type, description}` |
| `create_param_obj(props_obj, required_arr)` | 包装成 `{type:"object", properties, required}` |
| `create_tool_item(func_name, desc, params_obj)` | 包装成完整的工具项 |

参数 schema 的写法：

- 类型名用小写：`integer`、`number`、`string`、`boolean`、`object`、`array`。
- 整数用 `integer`，浮点用 `number`。两者在 cJSON 里都通过 `cJSON_IsNumber` 校验，区别在模型侧：`integer` 会倾向发整数。
- 数组要写 `items`，对象要写 `properties`，不要只写一个 `"type": "array"`。
- 必填参数放进 `required`，可选参数不要放，但执行函数要能处理缺失。
- `description` 写清含义、单位、范围和示例值。模型是否调用正确，很大程度取决于这段文字。

## 3. 添加工具

以 `add` 为例，共 4 步。

### 3.1 创建头文件

新建 `include/tools/tool_xxx.h`：

```c
#ifndef __AGENT_TOOL_XXX_H__
#define __AGENT_TOOL_XXX_H__

#include "tool_base.h"

void tool_xxx(cJSON *args_obj, AgentToolNode_t node);

#endif /* __AGENT_TOOL_XXX_H__ */
```

### 3.2 实现工具函数

新建 `src/tools/tool_xxx.c`：

```c
#include "tool_xxx.h"

void tool_xxx(cJSON *args_obj, AgentToolNode_t node)
{
    if (!args_obj || !node)
    {
        if (node)
        {
            if (node->ret.messages)
            {
                messages_destroy(node->ret.messages);
                node->ret.messages = RT_NULL;
            }
            node->ret.ret = RT_ERROR;
        }
        return;
    }

    /* 节点会复用，先释放上一次的结果 */
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    cJSON *a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON *b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");
    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    char ret_buffer[64] = {0};
    rt_snprintf(ret_buffer, sizeof(ret_buffer), "计算结果：%f",
                a_json->valuedouble + b_json->valuedouble);

    node->ret.messages = messages_create(1);
    if (node->ret.messages != RT_NULL)
    {
        messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    }
    node->ret.ret = (node->ret.messages != RT_NULL) ? RT_EOK : RT_ERROR;
}
```

参数校验失败时保持 `messages` 为 `RT_NULL`。框架会补一条“参数无效”的 tool 消息。

### 3.3 加入统一头文件

在 `include/tools/tools.h` 末尾追加：

```c
#include "tool_xxx.h"
```

### 3.4 在 init_tools() 中注册

编辑 `src/tool_func.c`：

```c
cJSON *props_xxx = cJSON_CreateObject();
create_property(props_xxx, "a", "number", "First number");
create_property(props_xxx, "b", "number", "Second number");

cJSON *required_xxx = cJSON_CreateArray();
cJSON_AddItemToArray(required_xxx, cJSON_CreateString("a"));
cJSON_AddItemToArray(required_xxx, cJSON_CreateString("b"));

cJSON *params_xxx = create_param_obj(props_xxx, required_xxx);
cJSON *xxx_func = create_tool_item("xxx", "Description of the tool", params_xxx);
append_tool(xxx_func, tool_xxx);
```

执行 `scons` 编译。`src/tools/*.c` 由 `SConscript` 自动收录，不需要改构建脚本。

## 4. 示例：求余工具 mod

**`include/tools/tool_mod.h`**

```c
#ifndef __AGENT_TOOL_MOD_H__
#define __AGENT_TOOL_MOD_H__

#include "tool_base.h"

void tool_mod(cJSON *args_obj, AgentToolNode_t node);

#endif /* __AGENT_TOOL_MOD_H__ */
```

**`src/tools/tool_mod.c`**

```c
#include "tool_mod.h"

void tool_mod(cJSON *args_obj, AgentToolNode_t node)
{
    if (!args_obj || !node)
    {
        if (node)
        {
            if (node->ret.messages)
            {
                messages_destroy(node->ret.messages);
                node->ret.messages = RT_NULL;
            }
            node->ret.ret = RT_ERROR;
        }
        return;
    }

    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    cJSON *a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON *b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");
    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    long a = (long)a_json->valuedouble;
    long b = (long)b_json->valuedouble;
    char ret_buffer[64] = {0};

    if (b == 0)
    {
        rt_snprintf(ret_buffer, sizeof(ret_buffer), "error: modulo by zero");
    }
    else
    {
        rt_snprintf(ret_buffer, sizeof(ret_buffer), "%ld %% %ld = %ld", a, b, a % b);
    }

    node->ret.messages = messages_create(1);
    if (node->ret.messages != RT_NULL)
    {
        messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    }
    node->ret.ret = (node->ret.messages != RT_NULL) ? RT_EOK : RT_ERROR;
}
```

注册代码：

```c
cJSON *props_mod = cJSON_CreateObject();
create_property(props_mod, "a", "number", "Dividend");
create_property(props_mod, "b", "number", "Divisor");

cJSON *required_mod = cJSON_CreateArray();
cJSON_AddItemToArray(required_mod, cJSON_CreateString("a"));
cJSON_AddItemToArray(required_mod, cJSON_CreateString("b"));

cJSON *params_mod = create_param_obj(props_mod, required_mod);
cJSON *mod_func = create_tool_item("mod", "Calculate the remainder of a divided by b", params_mod);
append_tool(mod_func, tool_mod);
```

## 5. 示例：字符串参数 word_count

参数读取方式与数值工具相同，只是校验函数换成 `cJSON_IsString`。

```c
cJSON *text_json = cJSON_GetObjectItemCaseSensitive(args_obj, "text");
if (!text_json || !cJSON_IsString(text_json))
{
    node->ret.ret = RT_ERROR;
    return;
}

const char *text = text_json->valuestring;
int words = 0;
int in_word = 0;

for (const char *p = text; *p != '\0'; p++)
{
    if (*p == ' ' || *p == '\t' || *p == '\n')
    {
        in_word = 0;
    }
    else if (!in_word)
    {
        in_word = 1;
        words++;
    }
}

char ret_buffer[64] = {0};
rt_snprintf(ret_buffer, sizeof(ret_buffer), "word count: %d", words);

node->ret.messages = messages_create(1);
if (node->ret.messages != RT_NULL)
{
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
}
node->ret.ret = (node->ret.messages != RT_NULL) ? RT_EOK : RT_ERROR;
```

注册：

```c
cJSON *props_wc = cJSON_CreateObject();
create_property(props_wc, "text", "string", "The text to count words in");

cJSON *required_wc = cJSON_CreateArray();
cJSON_AddItemToArray(required_wc, cJSON_CreateString("text"));

cJSON *params_wc = create_param_obj(props_wc, required_wc);
cJSON *wc_func = create_tool_item("word_count", "Count the number of words in a text", params_wc);
append_tool(wc_func, tool_word_count);
```

## 6. 注册与生命周期

- `init_tools()` 由 `init_agent()` 在进入对话时调用，也可以手动执行 MSH 命令 `init_tools`。
- 注册是幂等的，`init_flag` 保证只执行一次。
- `cleanup_agent()` 调用 `agent_tools_cleanup()`，释放工具链表、工具定义 JSON 与残留结果，并复位注册标志。
- 再次进入对话时会重新注册。工具代码不需要自己管理这些内存。
- MQTT 工具在 `PKG_AGENT_TOOL_MQTT_ENABLE` 打开时注册，其余工具始终注册。

## 7. 执行函数约定

1. 校验 `args_obj` 与 `node` 是否为 `RT_NULL`。
2. 每次执行前释放 `node->ret.messages`，节点会被复用。
3. 用 `cJSON_GetObjectItemCaseSensitive` 取字段，用 `cJSON_IsXxx` 校验类型。
4. 结果写入 `node->ret.messages`，文本使用 `TYPE_TEXT`。
5. 不要保存 `args_obj` 指针。`agent_loop` 在调用返回后会执行 `cJSON_Delete`。
6. 失败时可以把错误原因作为文本结果返回，也可以让 `messages` 保持 `RT_NULL`。后者会由框架补一条通用错误消息。

`node->ret.ret` 是工具侧的返回值约定，当前主循环不读取它。框架判断失败的依据是 `node->ret.messages == RT_NULL`。

工具结果只支持文本。`append_tool_message()` 只拼接 `TYPE_TEXT` 项，图片、音频等类型会被忽略。需要让模型看到文件时，返回文件路径或 URL 的文本。

## 8. 重复调用与轮次上限

框架对工具循环有四层限制：

- 同一次用户请求中，同一工具、同一参数再次出现时，不再执行。框架会返回一条“结果未变化，请直接回答”的 tool 消息。最近 6 条调用签名会被记住。
- 同一次用户请求中，同一工具调用超过 3 次时仍会执行，但计为无效调用。两组计数在每次 `agent_loop` 开始时清零。
- 连续两轮全部是重复或无效调用时，跳出工具循环，强制模型用文本回答。
- 工具循环最多 6 轮。达到上限后，再发一次不带工具的请求，要求模型给出最终回答。

因此工具结果要尽量包含有效信息。返回空字符串或只有一句“失败”，会让模型继续重试。

## 9. 参数细节

### 9.1 integer 与 number

| 类型 | 含义 | C 端取值 | 典型场景 |
|---|---|---|---|
| `integer` | 整数 | `valueint` | 计数、索引、ID、档位 |
| `number` | 浮点数 | `valuedouble` | 温度、比例、坐标、统计量 |

注意事项：

- cJSON 内部用 double 存储，两种类型都通过 `cJSON_IsNumber`。需要严格区分时，可以比较 `valueint` 与 `valuedouble`。
- `valueint` 的有效范围受平台 `int` 限制。大数用 `valuedouble` 接收，再自行转换。
- 模型偶尔会把数字包成字符串。执行函数必须用 `cJSON_IsNumber` 校验；如果经常出现，在描述里写明 `Please send a number, not a string`。
- 格式化时 `%d` 配 `valueint`，`%f` 配 `valuedouble`，不要混用。

### 9.2 布尔与字符串

- 布尔类型写 `"boolean"`，执行函数用 `cJSON_IsBool` 校验，用 `cJSON_IsTrue` / `cJSON_IsFalse` 判断。
- 字符串类型写 `"string"`，用 `cJSON_IsString` 校验，取 `valuestring`。
- 数组类型写 `"array"` 并给出 `items`，用 `cJSON_IsArray` 校验。
- 对象类型写 `"object"` 并给出 `properties`，用 `cJSON_IsObject` 校验。

### 9.3 长度限制

- 工具参数分片拼接受 `PKG_AGENT_MAX_TOOL_ARG_LEN` 限制，默认 1024 字节。超出后会告警并丢弃后续分片。
- SSE 单行受 `PKG_AGENT_STREAM_LINE_BUFSZ` 限制，默认 1024 字节。带长参数的工具调用如果整行被丢弃，模型侧会表现为“没有工具结果”，此时需要调大该值。
- 工具定义没有数量硬限制，但工具越多，每次请求的 payload 越大。按需注册。

## 10. 检查清单

- [ ] 新建 `include/tools/tool_xxx.h`，包含 `tool_base.h` 与函数声明。
- [ ] 新建 `src/tools/tool_xxx.c`，按约定校验参数、释放旧结果、写入文本结果。
- [ ] 在 `include/tools/tools.h` 添加 `#include "tool_xxx.h"`。
- [ ] 在 `src/tool_func.c` 的 `init_tools()` 中注册：properties → required → params → `create_tool_item` → `append_tool`。
- [ ] 执行 `scons` 编译。
- [ ] 用 `main_loop_entry` 进入对话，用自然语言触发工具。
- [ ] 反复执行“进入 → 对话 → 清理 → 再进入”，检查 `[mem] cleanup done` 的 `used` 是否持平。

## 11. 常见问题

**新增工具后没有生效？**

检查 `tools.h` 是否包含新头文件，`init_tools()` 是否调用了 `append_tool`，以及是否重新编译并重启。`init_flag` 在一次 agent 生命周期内保证只注册一次；`cleanup_agent()` 会复位该标志，再次进入时重新注册。

**模型不调用我的工具？**

先改工具描述和参数描述。写清何时使用、输入是什么、返回什么，并给出单位、范围和示例值。

**工具执行失败，结果如何反馈给模型？**

把错误原因作为 `TYPE_TEXT` 结果返回，或者让 `messages` 保持 `RT_NULL`，由框架补一条通用错误消息。模型会在下一轮看到这条结果。

**工具结果能影响后续对话吗？**

能。结果会写入上下文，计入 `PKG_AGENT_MESSAGE_TRIM` 的裁剪范围。裁剪只保留 system 消息和最近若干条消息，较早的工具结果可能被移除。

**多模态工具怎么返回图片？**

当前 `role=tool` 消息只接受文本，不能直接返回 `TYPE_IMAGE`。可以返回图片路径或上传后的 URL，让模型在回答中引用。
