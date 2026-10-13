# 工具开发指南：添加示例与自定义工具注册

> 面向开发者的工具扩展教程：如何为 Agent 框架添加新工具、如何自定义工具注册。
> 框架结构见 [framework.md](./framework.md)，API 见 [api.md](./api.md)。

---

## 1. 工具系统工作原理

Agent 的工具调用是**模型驱动**的闭环：

```
LLM 判定需要工具 → 返回 tool_calls（含函数名 + 参数 JSON）
        │
        ▼
agent_loop 按 function.name 查找已注册工具节点（search_agent_tool_node）
        │
        ▼
调用工具执行函数 tool_xxx(args_json, node)
        │
        ▼
工具把结果写入 node->ret.messages
        │
        ▼
结果以 role=tool 消息追加进上下文（append_tool_message）
        │
        ▼
进入下一轮 chat()，LLM 基于工具结果继续回答
```

**关键点**：
- 工具执行函数由你实现，签名固定：`void tool_xxx(cJSON *args, AgentToolNode_t node)`。
- 工具的 JSON 定义（名字、描述、参数 schema）在 `init_tools()` 中注册，会被发送给 LLM，**描述写得越好，模型调用越准**。
- 工具代码只需放在 `src/tools/*.c`，`SConscript` 自动收录（遍历目录），**无需修改构建脚本**。

---

## 2. 工具的 JSON 结构（LLM 视角）

每个工具最终以如下结构发送给模型（OpenAI function-calling 风格）：

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

框架提供三个工具函数帮助你构建这个结构（`include/utils.h`）：

| 函数 | 作用 |
|---|---|
| `create_property(props_obj, name, type, desc)` | 添加单个属性 `{type, description}` |
| `create_param_obj(props_obj, required_arr)` | 包装成 `{type:"object", properties, required}` |
| `create_tool_item(func_name, desc, params_obj)` | 包装成完整工具项 `{type:"function", function:{...}}` |

---

## 3. 添加新工具的标准流程

以内置工具 `add` 为模板，添加一个新工具共 **4 步**。

### 第 1 步：创建工具头文件

新建 `include/tools/tool_xxx.h`（xxx 为工具名）：

```c
#ifndef __AGENT_TOOL_XXX_H__
#define __AGENT_TOOL_XXX_H__
#include "tool_base.h"

/*
 * @brief 工具执行函数
 * @param args_obj 参数 JSON 对象
 * @param node     工具链表节点（用于返回结果）
 */
void tool_xxx(cJSON *args_obj, AgentToolNode_t node);

#endif // __AGENT_TOOL_XXX_H__
```

### 第 2 步：实现工具函数

新建 `src/tools/tool_xxx.c`，**必须遵循固定骨架**：

```c
#include "tool_xxx.h"

void tool_xxx(cJSON *args_obj, AgentToolNode_t node)
{
    /* 1. 参数校验：args 或 node 为空时兜底释放并置错误 */
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

    /* 2. 释放上一次调用的旧结果（节点会复用，防止消息堆积） */
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    /* 3. 解析并校验参数（必须检查字段存在性与类型） */
    cJSON *a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON *b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");
    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    /* 4. 业务计算 */
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    char ret_buffer[64] = {0};
    rt_snprintf(ret_buffer, sizeof(ret_buffer), "计算结果: %f", a + b);

    /* 5. 写入返回消息（文本结果用 TYPE_TEXT） */
    node->ret.messages = messages_create(1);
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}
```

### 第 3 步：加入统一头文件

在 `include/tools/tools.h` 中追加一行：

```c
#include "tool_add.h"
#include "tool_mul.h"
#include "tool_compare.h"
#include "tool_xxx.h"   /* ← 新增 */
```

### 第 4 步：在 `init_tools()` 中注册

编辑 `src/tool_func.c`，在 `init_tools()` 末尾追加注册代码：

```c
    /* ========== xxx ========== */
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

编译即可生效：`scons`（`src/tools/*.c` 自动收录，`include/tools` 已在 `CPPPATH`）。

---

## 4. 完整示例一：数值工具 `mod`（求余）

**`include/tools/tool_mod.h`**

```c
#ifndef __AGENT_TOOL_MOD_H__
#define __AGENT_TOOL_MOD_H__
#include "tool_base.h"

void tool_mod(cJSON *args_obj, AgentToolNode_t node);

#endif // __AGENT_TOOL_MOD_H__
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
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}
```

**`include/tools/tools.h`** 追加：

```c
#include "tool_mod.h"
```

**`src/tool_func.c` 注册**：

```c
    /* ========== mod ========== */
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

---

## 5. 完整示例二：字符串工具 `word_count`（统计单词数）

展示**字符串参数**与**多参数混合**的写法。

**`include/tools/tool_word_count.h`**

```c
#ifndef __AGENT_TOOL_WORD_COUNT_H__
#define __AGENT_TOOL_WORD_COUNT_H__
#include "tool_base.h"

void tool_word_count(cJSON *args_obj, AgentToolNode_t node);

#endif // __AGENT_TOOL_WORD_COUNT_H__
```

**`src/tools/tool_word_count.c`**

```c
#include "tool_word_count.h"

void tool_word_count(cJSON *args_obj, AgentToolNode_t node)
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

    cJSON *text_json = cJSON_GetObjectItemCaseSensitive(args_obj, "text");
    if (!text_json || !cJSON_IsString(text_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    const char *text = text_json->valuestring;
    int words = 0;
    int in_word = 0;
    for (const char *p = text; *p; p++)
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
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}
```

**`include/tools/tools.h`** 追加：

```c
#include "tool_word_count.h"
```

**`src/tool_func.c` 注册**：

```c
    /* ========== word_count ========== */
    cJSON *props_wc = cJSON_CreateObject();
    create_property(props_wc, "text", "string", "The text to count words in");

    cJSON *required_wc = cJSON_CreateArray();
    cJSON_AddItemToArray(required_wc, cJSON_CreateString("text"));

    cJSON *params_wc = create_param_obj(props_wc, required_wc);
    cJSON *wc_func = create_tool_item("word_count", "Count the number of words in a text", params_wc);
    append_tool(wc_func, tool_word_count);
```

---

## 6. 自定义工具注册的进阶说明

### 6.1 注册时机与生命周期

- `init_tools()` 由 `init_agent()` 自动调用（`main_loop_entry` 进入对话时），也可在 MSH 手动执行 `init_tools`。
- 注册是幂等的：`init_flag` 保护，重复调用直接返回。
- **清理与再注册**：`cleanup_agent()` 会调用 `agent_tools_cleanup()` 释放工具链表、工具 JSON 与节点残留消息，并复位 `init_flag`；再次进入对话时 `init_tools()` 自动重新注册。因此**不需要**（也不应）手动处理工具内存。

### 6.2 参数 schema 约定

#### 6.2.1 数值类型选型

OpenAI/JSON Schema 严格模式下,数值类参数只允许 `integer` 与 `number` 两种 `type`,两者在 cJSON 端都通过 `cJSON_IsNumber` 校验,但语义不同:

| `type` | 含义 | LLM 端 | C 端取值 | 典型场景 |
|---|---|---|---|---|
| `integer` | 整数,无小数部分 | int64 | `(long)x->valueint` | 计数、索引、ID、档位、循环次数 |
| `number`  | 浮点数,允许小数 | float64 | `(double)x->valuedouble` | 长度/重量/温度、比例、坐标、统计量 |

**关键差异**:

- 看到 `integer` 时 LLM 倾向发整数(避免 `5.0` 这类尾零);看到 `number` 可以发小数。
- cJSON 内部统一以 double 存储,**两种 `type` 都满足 `cJSON_IsNumber`**,单看校验无法区分整数/浮点。如需在执行函数中严格区分,可比较 `x->valueint != x->valuedouble`。
- `valueint` 在 cJSON 中为 `int`,32 位平台 ±2^31、64 位平台约 ±2^31;超界用 `valuedouble` 接收并自行强转 `int64_t` / `long long`。

#### 6.2.2 `integer` 注册与解析示例

```c
/* 注册 */
cJSON *props = cJSON_CreateObject();
create_property(props, "count", "integer", "How many items to process (>= 1)");
create_property(props, "page",  "integer", "Page index, 0-based");

/* 解析 */
cJSON *count = cJSON_GetObjectItemCaseSensitive(args, "count");
cJSON *page  = cJSON_GetObjectItemCaseSensitive(args, "page");
if (!count || !page || !cJSON_IsNumber(count) || !cJSON_IsNumber(page))
{
    node->ret.ret = RT_ERROR;
    return;
}
long n   = (long)count->valueint;
long idx = (long)page->valueint;
if (n < 1 || idx < 0) { /* 业务校验 */ }
```

#### 6.2.3 `number` 注册与解析示例

```c
/* 注册 */
cJSON *props = cJSON_CreateObject();
create_property(props, "voltage",   "number", "Measured voltage in volts (e.g. 3.3)");
create_property(props, "threshold", "number", "Threshold to compare against, range 0.0 - 1.0");

/* 解析 */
cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "voltage");
cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "threshold");
if (!v || !t || !cJSON_IsNumber(v) || !cJSON_IsNumber(t))
{
    node->ret.ret = RT_ERROR;
    return;
}
double voltage   = v->valuedouble;
double threshold = t->valuedouble;
```

#### 6.2.4 常见误区

- **不要混用**:`number` 接整数浪费精度,`integer` 接小数会被 LLM 四舍五入,边界处极易翻车。
- **不要传字符串数字**:LLM 偶尔把数字包成 `"3.3"` 字符串,即便 schema 是 `number` 也会出现。执行函数必须 `cJSON_IsNumber` 校验;若模型反复误发,在 description 里强调 `Please send a number, not a string`。
- **大数/精度**:`integer` 超过 2^53、`number` 超过 15~16 位有效数字会丢精度,业务侧先确认范围再选 `type`。
- **格式化输出**:`%d` 配 `valueint`,`%f` / `%lf` 配 `valuedouble`,别混。

#### 6.2.5 其它类型与约定

- 文本用 `"string"`,执行函数用 `cJSON_IsString` 校验,取 `valuestring`。
- 布尔用 `"true"` / `"false"`(备注:OpenAI 严格 schema 中布尔 `type` 应写 `"boolean"`,此处保留现有 cJSON 风格;若切到 strict 模式需要再统一改),执行函数用 `cJSON_IsBool` 校验,取 `valueint` 判 0/非 0。
- 必填参数务必加入 `required` 数组;可选参数不加入,但执行函数内要容忍缺失。
- 属性描述(description)与工具描述请写清楚,直接影响模型调用的准确率;数值参数最好在 description 里写出**单位 / 范围 / 示例值**。

### 6.3 执行函数的硬性要求

1. **参数与节点校验**：`args_obj`/`node` 为 NULL 时兜底释放并返回。
2. **先释放旧消息**：节点会被复用（同一工具多次调用），每次执行前 `messages_destroy(node->ret.messages)`。
3. **参数类型校验**：用 `cJSON_GetObjectItemCaseSensitive` + `cJSON_IsXxx` 检查，缺失/类型错误置 `node->ret.ret = RT_ERROR` 返回。
4. **结果写入**：`node->ret.messages = messages_create(1); messages_append(..., TYPE_TEXT, 文本);`，`node->ret.ret = RT_EOK`。
5. **不要保留 args_obj 指针**：`args_obj` 由 `agent_loop` 在执行后释放（`cJSON_Delete(args_json)`），工具内如需长时间保存请自行拷贝。

### 6.4 返回消息类型

`Messages_t` 支持多种类型（`include/MessageHub.h`）：

| 枚举 | 用途 |
|---|---|
| `TYPE_TEXT` | 文本结果（绝大多数工具） |
| `TYPE_AUDIO` / `TYPE_IMAGE` / `TYPE_VIDEO` | 多模态结果（启用 `PKG_AGENT_MULTIMODAL_ENABLE` 时，`to_content` 会自动上传并转为 URL） |

多模态工具示例（返回图片路径）：

```c
node->ret.messages = messages_create(1);
messages_append(node->ret.messages, TYPE_IMAGE, "/sd/picture.png");
node->ret.ret = RT_EOK;
```

### 6.5 工具数量与参数长度

- 工具参数分片拼接受 `PKG_AGENT_MAX_TOOL_ARG_LEN` 限制（超限会告警并跳过追加）。
- 工具数量无硬限制，但注册过多会显著增大每次请求的 payload，建议按需注册。

---

## 7. 完整流程速查（Checklist）

- [ ] 新建 `include/tools/tool_xxx.h`（含 `tool_base.h` + 函数声明）
- [ ] 新建 `src/tools/tool_xxx.c`（按 §3 骨架实现）
- [ ] 在 `include/tools/tools.h` 追加 `#include "tool_xxx.h"`
- [ ] 在 `src/tool_func.c::init_tools()` 追加注册代码（properties → required → params → create_tool_item → append_tool）
- [ ] `scons` 编译（构建脚本无需修改）
- [ ] 运行时用 `main_loop_entry` 进入对话，用自然语言触发新工具验证
- [ ] 反复"进入→对话→清理→再进入"，确认无内存增长（`[mem] cleanup done` 日志）

---

## 8. FAQ

**Q1：新增工具后没有生效？**
检查：`tools.h` 是否包含新头文件；`init_tools()` 是否调用 `append_tool`；是否重新编译并重启（`init_flag` 在同一进程内只注册一次，若中途改代码需重新烧录）。也可在 MSH 执行 `init_tools` 手动触发。

**Q2：模型不调用我的工具？**
多为描述/参数 schema 质量问题：工具描述要说明"何时使用、输入什么、返回什么"；参数 description 要写清含义；必要时把示例值写进 description。

**Q3：工具执行失败，结果如何反馈给模型？**
把错误信息作为 `TYPE_TEXT` 结果写入 `node->ret.messages`（`node->ret.ret = RT_ERROR`），模型会看到错误原因并尝试其他方案（框架系统提示词已要求工具失败时分析原因）。

**Q4：工具结果能影响后续对话吗？**
能。结果会以 `role=tool` 消息追加进上下文，LLM 在下一轮基于它继续回答，并计入 `PKG_AGENT_MESSAGE_TRIM` 的裁剪范围。

**Q5：多模态工具的文件路径规则？**
`TYPE_IMAGE` 等消息内容为本地文件路径，`to_content` 会调用 `agent_up_load` 上传到文件服务器并转为 URL；上传失败该条会被跳过并打日志。
