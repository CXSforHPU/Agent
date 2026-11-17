#include "utils/utils_internal.h"

#define LOG_TAG "Agent.protocol"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

const agent_protocol_ops_t *agent_protocol_openai_ops(void);
const agent_protocol_ops_t *agent_protocol_anthropic_ops(void);

/*
 * @brief 取当前协议的操作表
 * @return 操作表指针
 */
const agent_protocol_ops_t *agent_protocol_get_ops(void)
{
    if (agent_protocol_current() == AGENT_API_PROTOCOL_ANTHROPIC)
    {
        return agent_protocol_anthropic_ops();
    }
    return agent_protocol_openai_ops();
}

/*
 * @brief 取当前协议
 * @return 协议枚举
 */
agent_api_protocol_t agent_protocol_current(void)
{
    return get_dynamic_agent_api_protocol();
}

/*
 * @brief 切换协议
 * @param protocol 协议枚举
 */
void agent_protocol_switch(agent_api_protocol_t protocol)
{
    agent_config_set_protocol(protocol);
    LOG_I("agent protocol switched to %s",
          (protocol == AGENT_API_PROTOCOL_ANTHROPIC) ? "anthropic" : "openai");
}

/*
 * @brief 打印或切换 LLM 协议
 * @param argc 参数个数
 * @param argv 参数数组
 * @return RT_EOK
 */
static int agent_protocol(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("agent protocol: %s\n",
                   (agent_protocol_current() == AGENT_API_PROTOCOL_ANTHROPIC) ? "anthropic" : "openai");
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "openai") == 0)
    {
        agent_protocol_switch(AGENT_API_PROTOCOL_OPENAI);
    }
    else if (rt_strcmp(argv[1], "anthropic") == 0)
    {
        agent_protocol_switch(AGENT_API_PROTOCOL_ANTHROPIC);
    }
    else
    {
        LOG_E("usage: agent_protocol [openai|anthropic]");
        return -RT_EINVAL;
    }

    rt_kprintf("agent protocol: %s\n",
               (agent_protocol_current() == AGENT_API_PROTOCOL_ANTHROPIC) ? "anthropic" : "openai");
    return RT_EOK;
}
MSH_CMD_EXPORT(agent_protocol, switch LLM protocol: openai|anthropic);

/*
 * @brief 初始化流状态
 * @param stream        流状态
 * @param on_reasoning  思考过程回调
 * @param on_tool_call  工具调用回调
 * @param on_context    正文回调
 */
void agent_protocol_stream_init(agent_protocol_stream_t *stream,
                                 void (*on_reasoning)(const char *text),
                                 void (*on_tool_call)(const char *text),
                                 void (*on_context)(const char *text))
{
    rt_memset(stream, 0, sizeof(agent_protocol_stream_t));

    stream->reasoning = (char *)rt_malloc(AGENT_PROTOCOL_MAX_REASONING_LEN);
    stream->content = (char *)rt_malloc(AGENT_PROTOCOL_MAX_CONTENT_LEN);
    stream->tool_calls = cJSON_CreateArray();
    stream->tools = RT_NULL;

    if (stream->reasoning)
    {
        stream->reasoning[0] = '\0';
    }
    if (stream->content)
    {
        stream->content[0] = '\0';
    }

    stream->on_reasoning = on_reasoning;
    stream->on_tool_call = on_tool_call;
    stream->on_context = on_context;
}

/*
 * @brief 释放流状态
 * @param stream 流状态
 */
void agent_protocol_stream_deinit(agent_protocol_stream_t *stream)
{
    int i;

    if (!stream) return;

    if (stream->reasoning) rt_free(stream->reasoning);
    if (stream->content) rt_free(stream->content);
    if (stream->tool_calls) cJSON_Delete(stream->tool_calls);
    if (stream->tools)
    {
        for (i = 0; i < stream->tool_count; i++)
        {
            if (stream->tools[i].args) rt_free(stream->tools[i].args);
        }
        rt_free(stream->tools);
    }

    stream->reasoning = RT_NULL;
    stream->content = RT_NULL;
    stream->tool_calls = RT_NULL;
    stream->tools = RT_NULL;
    stream->tool_count = 0;
}

/*
 * @brief 追加思考过程
 * @param stream 流状态
 * @param seg    文本分片
 */
void agent_protocol_add_reasoning(agent_protocol_stream_t *stream, const char *seg)
{
    size_t seg_len;
    int remain;

    if (!stream || !seg || !seg[0] || !stream->reasoning) return;

    remain = AGENT_PROTOCOL_MAX_REASONING_LEN - stream->reasoning_len - 1;
    if (remain <= 0) return;

    seg_len = rt_strlen(seg);
    if ((int)seg_len > remain) seg_len = remain;

    rt_strncpy(stream->reasoning + stream->reasoning_len, seg, seg_len);
    stream->reasoning_len += seg_len;
    stream->reasoning[stream->reasoning_len] = '\0';

    if (stream->on_reasoning)
    {
        stream->on_reasoning(seg);
    }
}

/*
 * @brief 追加回答正文
 * @param stream 流状态
 * @param seg    文本分片
 */
void agent_protocol_add_content(agent_protocol_stream_t *stream, const char *seg)
{
    size_t seg_len;
    int remain;

    if (!stream || !seg || !seg[0] || !stream->content) return;

    if (!stream->is_answering)
    {
        if (stream->on_context) rt_kprintf("\n");
        stream->is_answering = RT_TRUE;
    }

    remain = AGENT_PROTOCOL_MAX_CONTENT_LEN - stream->content_len - 1;
    if (remain <= 0)
    {
        stream->content_lost = RT_TRUE;
        return;
    }

    seg_len = rt_strlen(seg);
    if ((int)seg_len > remain)
    {
        seg_len = remain;
        stream->content_lost = RT_TRUE;
    }

    rt_strncpy(stream->content + stream->content_len, seg, seg_len);
    stream->content_len += seg_len;
    stream->content[stream->content_len] = '\0';

    if (stream->on_context)
    {
        stream->on_context(seg);
    }
}

/*
 * @brief 按 index 找工具调用状态，没有就新开一个
 * @param stream 流状态
 * @param index  协议里的调用序号
 * @return 调用状态，超过上限返回 NULL
 */
static agent_protocol_tool_t *__tool_slot(agent_protocol_stream_t *stream, int index)
{
    agent_protocol_tool_t *slot;
    int i;

    /* 槽表在第一次出现工具调用时才分配，纯文本轮次不占这块内存 */
    if (!stream->tools)
    {
        stream->tools = (agent_protocol_tool_t *)rt_calloc(AGENT_PROTOCOL_MAX_TOOL_CALLS,
                                                           sizeof(agent_protocol_tool_t));
        if (!stream->tools) return RT_NULL;
    }

    for (i = 0; i < stream->tool_count; i++)
    {
        if (stream->tools[i].index == index)
        {
            return &stream->tools[i];
        }
    }

    if (stream->tool_count >= AGENT_PROTOCOL_MAX_TOOL_CALLS)
    {
        LOG_W("too many tool calls, index %d dropped\n", index);
        return RT_NULL;
    }

    slot = &stream->tools[stream->tool_count++];
    rt_memset(slot, 0, sizeof(*slot));
    slot->index = index;

    return slot;
}

/*
 * @brief 登记一个工具调用的 id 与函数名
 * @param stream 流状态
 * @param index  分片序号
 * @param id     工具调用 id
 * @param name   函数名
 */
void agent_protocol_tool_begin(agent_protocol_stream_t *stream, int index, const char *id, const char *name)
{
    agent_protocol_tool_t *slot;

    if (!stream) return;
    if ((!id || !id[0]) && (!name || !name[0])) return;

    slot = __tool_slot(stream, index);
    if (!slot) return;

    /* 只有首个分片携带 id 与函数名，后续分片保持已有值 */
    if (id && id[0] && !slot->id[0])
    {
        rt_strncpy(slot->id, id, sizeof(slot->id) - 1);
    }
    if (name && name[0] && !slot->name[0])
    {
        rt_strncpy(slot->name, name, sizeof(slot->name) - 1);
    }
}

/*
 * @brief 追加工具调用参数分片
 * @param stream   流状态
 * @param index    分片序号
 * @param fragment 参数分片
 */
void agent_protocol_tool_args(agent_protocol_stream_t *stream, int index, const char *fragment)
{
    agent_protocol_tool_t *slot;
    int frag_len;
    int need;
    int cap;
    char *grown;

    if (!stream || !fragment || !fragment[0]) return;

    slot = __tool_slot(stream, index);
    if (!slot) return;

    frag_len = (int)rt_strlen(fragment);
    if (slot->args_len + frag_len >= AGENT_PROTOCOL_MAX_TOOL_ARG_LEN)
    {
        LOG_W("tool arguments exceed limit, skip append\n");
        return;
    }

    need = slot->args_len + frag_len + 1;
    if (need > slot->args_cap)
    {
        cap = slot->args_cap > 0 ? slot->args_cap : 128;
        while (cap < need)
        {
            cap *= 2;
        }
        grown = (char *)rt_realloc(slot->args, cap);
        if (grown == RT_NULL)
        {
            LOG_E("tool args realloc fail\n");
            return;
        }
        slot->args = grown;
        slot->args_cap = cap;
    }

    rt_memcpy(slot->args + slot->args_len, fragment, (rt_size_t)frag_len);
    slot->args_len += frag_len;
    slot->args[slot->args_len] = '\0';
}

rt_bool_t agent_protocol_text_append(char *buf, int cap, int *len, const char *text, int text_len)
{
    rt_bool_t truncated = RT_FALSE;
    int remain;

    if (!buf || !len || !text || text_len <= 0 || cap <= 0) return RT_FALSE;

    remain = cap - 1 - *len;
    if (remain <= 0)
    {
        return RT_TRUE;
    }
    if (text_len > remain)
    {
        text_len = remain;
        truncated = RT_TRUE;
    }

    rt_memcpy(buf + *len, text, (rt_size_t)text_len);
    *len += text_len;
    buf[*len] = '\0';

    return truncated;
}

void agent_protocol_tool_field(agent_protocol_stream_t *stream, int index,
                               char *buf, int cap, int *len,
                               const char *text, int text_len,
                               rt_bool_t last, rt_bool_t is_id)
{
    if (agent_protocol_text_append(buf, cap, len, text, text_len))
    {
        stream->content_lost = RT_TRUE;
    }
    if (last)
    {
        agent_protocol_tool_begin(stream, index,
                                  is_id ? buf : RT_NULL,
                                  is_id ? RT_NULL : buf);
        *len = 0;
    }
}

/*
 * @brief 把分片归一化成 tool_calls 数组并回调
 * @param stream 流状态
 */
void agent_protocol_tool_finalize(agent_protocol_stream_t *stream)
{
    int i;

    if (!stream || !stream->tools || !stream->tool_calls) return;

    /*
     * 归一化规则：
     * - id 缺失时用 call_<index> 兜底，保证下游 tool_call_id 始终有效
     * - arguments 缺失或为空时用 "{}" 兜底，避免下游 cJSON_Parse 失败
     * - 无函数名的条目视为无效，直接丢弃
     */
    for (i = 0; i < stream->tool_count; i++)
    {
        agent_protocol_tool_t *slot = &stream->tools[i];
        const char *func_name = slot->name;
        const char *args_str = (slot->args && slot->args_len > 0) ? slot->args : "{}";
        char id_buf[32] = {0};
        const char *call_id;
        cJSON *item;
        cJSON *func_obj;

        if (func_name[0] == '\0')
        {
            LOG_W("tool call without function name, dropped\n");
            continue;
        }

        call_id = slot->id[0] ? slot->id : RT_NULL;
        if (call_id == RT_NULL)
        {
            rt_snprintf(id_buf, sizeof(id_buf), "call_%d", slot->index);
            call_id = id_buf;
        }

        item = cJSON_CreateObject();
        func_obj = cJSON_CreateObject();
        if (item == RT_NULL || func_obj == RT_NULL)
        {
            if (item) cJSON_Delete(item);
            if (func_obj) cJSON_Delete(func_obj);
            LOG_E("create tool call item fail\n");
            break;
        }
        cJSON_AddStringToObject(item, "id", call_id);
        cJSON_AddStringToObject(item, "type", "function");
        cJSON_AddStringToObject(func_obj, "name", func_name);
        cJSON_AddStringToObject(func_obj, "arguments", args_str);
        cJSON_AddItemToObject(item, "function", func_obj);
        cJSON_AddItemToArray(stream->tool_calls, item);
    }

    if (stream->on_tool_call)
    {
        int tool_cnt = cJSON_GetArraySize(stream->tool_calls);

        for (i = 0; i < tool_cnt; i++)
        {
            cJSON *tool_item = cJSON_GetArrayItem(stream->tool_calls, i);
            char *tool_json = cJSON_PrintUnformatted(tool_item);

            if (tool_json)
            {
                stream->on_tool_call(tool_json);
                cJSON_free(tool_json);
            }
        }
    }
}
