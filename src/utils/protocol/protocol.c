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
        rt_kprintf("usage: agent_protocol [openai|anthropic]\n");
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
    stream->tool_call_map = cJSON_CreateObject();

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
    if (!stream) return;

    if (stream->reasoning) rt_free(stream->reasoning);
    if (stream->content) rt_free(stream->content);
    if (stream->tool_calls) cJSON_Delete(stream->tool_calls);
    if (stream->tool_call_map) cJSON_Delete(stream->tool_call_map);

    stream->reasoning = RT_NULL;
    stream->content = RT_NULL;
    stream->tool_calls = RT_NULL;
    stream->tool_call_map = RT_NULL;
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
 * @brief 登记一个工具调用的 id 与函数名
 * @param stream 流状态
 * @param index  分片序号
 * @param id     工具调用 id
 * @param name   函数名
 */
void agent_protocol_tool_begin(agent_protocol_stream_t *stream, int index, const char *id, const char *name)
{
    char key_buf[16] = {0};
    cJSON *map_entry;

    if (!stream || !stream->tool_call_map) return;

    rt_snprintf(key_buf, sizeof(key_buf), "%d", index);

    map_entry = cJSON_GetObjectItemCaseSensitive(stream->tool_call_map, key_buf);
    if (!map_entry)
    {
        map_entry = cJSON_CreateObject();
        if (!map_entry) return;
        cJSON_AddNumberToObject(map_entry, "index", index);
        cJSON_AddStringToObject(map_entry, "_id", "");
        cJSON_AddStringToObject(map_entry, "_name", "");
        cJSON_AddStringToObject(map_entry, "arguments", "");
        cJSON_AddItemToObject(stream->tool_call_map, key_buf, map_entry);
    }

    /* 只有首个分片携带 id 与函数名，后续分片保持已有值 */
    if (id && id[0] != '\0')
    {
        cJSON *exist_id = cJSON_GetObjectItemCaseSensitive(map_entry, "_id");
        if (!exist_id || exist_id->valuestring == NULL || exist_id->valuestring[0] == '\0')
        {
            cJSON_ReplaceItemInObjectCaseSensitive(
                map_entry, "_id", cJSON_CreateString(id));
        }
    }

    if (name && name[0] != '\0')
    {
        cJSON *exist_name = cJSON_GetObjectItemCaseSensitive(map_entry, "_name");
        if (!exist_name || exist_name->valuestring == NULL || exist_name->valuestring[0] == '\0')
        {
            cJSON_ReplaceItemInObjectCaseSensitive(
                map_entry, "_name", cJSON_CreateString(name));
        }
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
    char key_buf[16] = {0};
    cJSON *map_entry;
    cJSON *exist_args;
    const char *old_str;
    size_t old_len;
    size_t append_len;
    size_t total_len;
    char *combine_buf;

    if (!stream || !stream->tool_call_map || !fragment) return;
    if (fragment[0] == '\0') return;

    rt_snprintf(key_buf, sizeof(key_buf), "%d", index);

    map_entry = cJSON_GetObjectItemCaseSensitive(stream->tool_call_map, key_buf);
    if (!map_entry)
    {
        agent_protocol_tool_begin(stream, index, RT_NULL, RT_NULL);
        map_entry = cJSON_GetObjectItemCaseSensitive(stream->tool_call_map, key_buf);
        if (!map_entry) return;
    }

    exist_args = cJSON_GetObjectItemCaseSensitive(map_entry, "arguments");
    if (!exist_args || exist_args->valuestring == NULL || exist_args->valuestring[0] == '\0')
    {
        cJSON_ReplaceItemInObjectCaseSensitive(
            map_entry, "arguments", cJSON_CreateString(fragment));
        return;
    }

    old_str = exist_args->valuestring;
    old_len = rt_strlen(old_str);
    append_len = rt_strlen(fragment);
    total_len = old_len + append_len;

    if (total_len >= AGENT_PROTOCOL_MAX_TOOL_ARG_LEN)
    {
        LOG_W("tool arguments exceed limit, skip append\n");
        return;
    }

    combine_buf = rt_malloc(total_len + 1);
    if (combine_buf == RT_NULL)
    {
        LOG_E("tool args combine malloc fail\n");
        return;
    }
    rt_memcpy(combine_buf, old_str, old_len);
    rt_memcpy(combine_buf + old_len, fragment, append_len);
    combine_buf[total_len] = '\0';

    cJSON_ReplaceItemInObjectCaseSensitive(
        map_entry, "arguments", cJSON_CreateString(combine_buf));
    rt_free(combine_buf);
}

/*
 * @brief 把分片归一化成 tool_calls 数组并回调
 * @param stream 流状态
 */
void agent_protocol_tool_finalize(agent_protocol_stream_t *stream)
{
    cJSON *map_key;

    if (!stream || !stream->tool_call_map || !stream->tool_calls) return;

    /*
     * 归一化规则：
     * - id 缺失时用 call_<index> 兜底，保证下游 tool_call_id 始终有效
     * - arguments 缺失或为空时用 "{}" 兜底，避免下游 cJSON_Parse 失败
     * - 无函数名的条目视为无效，直接丢弃
     */
    for (map_key = stream->tool_call_map->child; map_key != NULL; map_key = map_key->next)
    {
        cJSON *entry = map_key;
        cJSON *id_node = cJSON_GetObjectItemCaseSensitive(entry, "_id");
        cJSON *name_node = cJSON_GetObjectItemCaseSensitive(entry, "_name");
        cJSON *args_node = cJSON_GetObjectItemCaseSensitive(entry, "arguments");
        cJSON *idx_node = cJSON_GetObjectItemCaseSensitive(entry, "index");
        const char *func_name = (name_node && cJSON_IsString(name_node)) ? name_node->valuestring : RT_NULL;
        const char *args_str;
        const char *call_id;
        char id_buf[32] = {0};
        cJSON *item;
        cJSON *func_obj;

        if (func_name == RT_NULL || func_name[0] == '\0')
        {
            LOG_W("tool call without function name, dropped\n");
            continue;
        }

        args_str = (args_node && cJSON_IsString(args_node) && args_node->valuestring[0] != '\0')
                   ? args_node->valuestring : "{}";

        call_id = (id_node && cJSON_IsString(id_node) && id_node->valuestring[0] != '\0')
                  ? id_node->valuestring : RT_NULL;
        if (call_id == RT_NULL)
        {
            rt_snprintf(id_buf, sizeof(id_buf), "call_%d",
                        (idx_node && cJSON_IsNumber(idx_node)) ? idx_node->valueint : 0);
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

        for (int i = 0; i < tool_cnt; i++)
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
