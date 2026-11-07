#include "utils/utils_internal.h"

#define LOG_TAG "Agent.protocol.anthropic"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#ifndef PKG_AGENT_ANTHROPIC_VERSION
#define PKG_AGENT_ANTHROPIC_VERSION "2023-06-01"
#endif

#ifndef PKG_AGENT_ANTHROPIC_MAX_TOKENS
#define PKG_AGENT_ANTHROPIC_MAX_TOKENS 4096
#endif

/*
 * Anthropic 协议：POST /v1/messages
 * 请求体 {model, max_tokens, system, messages, tools, stream}，
 * 鉴权用 x-api-key 头，另带 anthropic-version 头。
 * 响应是事件式 SSE：content_block_start / content_block_delta /
 * content_block_stop / message_delta / message_stop，以 message_stop 收尾。
 *
 * 与规范格式（OpenAI 风格）的对应关系：
 *   system 消息      -> 顶层 system 字段
 *   user 消息        -> content 块数组
 *   assistant 消息   -> content 块数组，工具调用变成 tool_use 块
 *   tool 消息        -> user 消息里的 tool_result 块
 *   tools 定义       -> name / description / input_schema
 */

/*
 * @brief 往动态缓冲区尾部追加文本
 * @param buf   缓冲区
 * @param len   当前长度
 * @param cap   容量
 * @param text  要追加的文本
 * @return RT_EOK 成功
 */
static rt_err_t append_text(char **buf, rt_size_t *len, rt_size_t *cap, const char *text)
{
    rt_size_t text_len;
    rt_size_t need;
    char *new_buf;

    if (!buf || !len || !cap || !text) return -RT_EINVAL;

    text_len = rt_strlen(text);
    need = *len + text_len + 1;
    if (need > *cap)
    {
        rt_size_t new_cap = (*cap == 0) ? 256 : *cap;

        while (new_cap < need)
        {
            new_cap *= 2;
        }
        new_buf = rt_realloc(*buf, new_cap);
        if (!new_buf) return -RT_ENOMEM;
        *buf = new_buf;
        *cap = new_cap;
    }

    rt_memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';

    return RT_EOK;
}

/*
 * @brief 取字符串字段
 * @param obj   对象
 * @param key   键名
 * @return 字符串指针，对象里没有或不是字符串时返回 NULL
 */
static const char *get_string(cJSON *obj, const char *key)
{
    cJSON *item;

    if (!obj) return RT_NULL;
    item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item)) return RT_NULL;

    return item->valuestring;
}

/*
 * @brief 把规范格式的 content 块数组转成 Anthropic 的块数组
 * @param content 规范格式的 content，字符串或块数组
 * @return Anthropic 的 content 块数组
 */
static cJSON *anthropic_format_content(cJSON *content)
{
    cJSON *blocks;
    int size;
    int i;

    blocks = cJSON_CreateArray();
    if (!blocks) return RT_NULL;

    if (!content)
    {
        cJSON_AddItemToArray(blocks, cJSON_CreateString(""));
        return blocks;
    }

    if (cJSON_IsString(content))
    {
        cJSON *text = cJSON_CreateObject();

        if (text)
        {
            cJSON_AddStringToObject(text, "type", "text");
            cJSON_AddStringToObject(text, "text", content->valuestring);
            cJSON_AddItemToArray(blocks, text);
        }
        return blocks;
    }

    if (!cJSON_IsArray(content))
    {
        return blocks;
    }

    size = cJSON_GetArraySize(content);
    for (i = 0; i < size; i++)
    {
        cJSON *item = cJSON_GetArrayItem(content, i);
        const char *type;

        if (!cJSON_IsObject(item)) continue;

        type = get_string(item, "type");
        if (type && rt_strcmp(type, "text") == 0)
        {
            const char *text = get_string(item, "text");
            cJSON *block = cJSON_CreateObject();

            if (block)
            {
                cJSON_AddStringToObject(block, "type", "text");
                cJSON_AddStringToObject(block, "text", text ? text : "");
                cJSON_AddItemToArray(blocks, block);
            }
        }
        else if (type && rt_strcmp(type, "image_url") == 0)
        {
            /* 规范格式里图片是 {"type":"image_url","image_url":{"url":...}} */
            cJSON *url_obj = cJSON_GetObjectItemCaseSensitive(item, "image_url");
            const char *url = get_string(url_obj, "url");
            cJSON *block;

            if (!url || !url[0]) continue;

            block = cJSON_CreateObject();
            if (!block) continue;
            cJSON_AddStringToObject(block, "type", "image");
            {
                cJSON *source = cJSON_CreateObject();

                if (source)
                {
                    cJSON_AddStringToObject(source, "type", "url");
                    cJSON_AddStringToObject(source, "url", url);
                    cJSON_AddItemToObject(block, "source", source);
                }
            }
            cJSON_AddItemToArray(blocks, block);
        }
        else if (type && (rt_strcmp(type, "audio_url") == 0 || rt_strcmp(type, "video_url") == 0))
        {
            /* Anthropic 的 Messages 接口不收内联音视频，换成一句说明 */
            cJSON *block = cJSON_CreateObject();

            LOG_W("anthropic: %s block not supported, replaced by a text note", type);
            if (block)
            {
                cJSON_AddStringToObject(block, "type", "text");
                cJSON_AddStringToObject(block, "text",
                                        "[音视频内容在 Anthropic 协议下未发送]");
                cJSON_AddItemToArray(blocks, block);
            }
        }
    }

    return blocks;
}

/*
 * @brief 把 OpenAI 风格的工具调用转成 Anthropic 的 tool_use 块
 * @param tool_call 工具调用对象
 * @return tool_use 块
 */
static cJSON *anthropic_tool_use_block(cJSON *tool_call)
{
    cJSON *func = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
    const char *id = get_string(tool_call, "id");
    const char *name = get_string(func, "name");
    const char *arguments = get_string(func, "arguments");
    cJSON *block = cJSON_CreateObject();

    if (!block) return RT_NULL;

    cJSON_AddStringToObject(block, "type", "tool_use");
    cJSON_AddStringToObject(block, "id", id ? id : "");
    cJSON_AddStringToObject(block, "name", name ? name : "");

    if (arguments && arguments[0])
    {
        cJSON *input = cJSON_Parse(arguments);

        if (input && cJSON_IsObject(input))
        {
            cJSON_AddItemToObject(block, "input", input);
        }
        else
        {
            if (input) cJSON_Delete(input);
            cJSON_AddItemToObject(block, "input", cJSON_CreateObject());
        }
    }
    else
    {
        cJSON_AddItemToObject(block, "input", cJSON_CreateObject());
    }

    return block;
}

/*
 * @brief 判断数组最后一条是不是 tool_result 组成的 user 消息
 * @param messages Anthropic 消息数组
 * @return RT_TRUE 是
 */
static rt_bool_t last_is_tool_result_message(cJSON *messages)
{
    cJSON *last = cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1);
    cJSON *content;
    cJSON *first;
    const char *type;

    if (!last || !cJSON_IsObject(last)) return RT_FALSE;
    if (!get_string(last, "role") || rt_strcmp(get_string(last, "role"), "user") != 0)
    {
        return RT_FALSE;
    }

    content = cJSON_GetObjectItemCaseSensitive(last, "content");
    if (!cJSON_IsArray(content)) return RT_FALSE;

    first = cJSON_GetArrayItem(content, 0);
    type = get_string(first, "type");

    return (type && rt_strcmp(type, "tool_result") == 0) ? RT_TRUE : RT_FALSE;
}

/*
 * @brief 把规范格式的消息数组转成 Anthropic 的消息数组
 * @param messages   规范格式的消息数组
 * @param system_out system 文本，调用者负责释放
 * @return Anthropic 消息数组
 */
static cJSON *anthropic_translate_messages(cJSON *messages, char **system_out)
{
    cJSON *out = cJSON_CreateArray();
    char *system_buf = RT_NULL;
    rt_size_t system_len = 0;
    rt_size_t system_cap = 0;
    int size;
    int i;

    if (!out) return RT_NULL;
    *system_out = RT_NULL;

    if (!messages || !cJSON_IsArray(messages))
    {
        return out;
    }

    size = cJSON_GetArraySize(messages);
    for (i = 0; i < size; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        const char *role;
        cJSON *content;

        if (!cJSON_IsObject(msg)) continue;

        role = get_string(msg, "role");
        content = cJSON_GetObjectItemCaseSensitive(msg, "content");

        if (!role) continue;

        if (rt_strcmp(role, "system") == 0)
        {
            const char *text = cJSON_IsString(content) ? content->valuestring : RT_NULL;

            if (!text) continue;
            if (system_len > 0)
            {
                append_text(&system_buf, &system_len, &system_cap, "\n\n");
            }
            append_text(&system_buf, &system_len, &system_cap, text);
        }
        else if (rt_strcmp(role, "user") == 0)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON *blocks = anthropic_format_content(content);

            if (!item || !blocks)
            {
                if (item) cJSON_Delete(item);
                if (blocks) cJSON_Delete(blocks);
                continue;
            }
            cJSON_AddStringToObject(item, "role", "user");
            cJSON_AddItemToObject(item, "content", blocks);
            cJSON_AddItemToArray(out, item);
        }
        else if (rt_strcmp(role, "assistant") == 0)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON *blocks = cJSON_CreateArray();
            cJSON *tool_calls;
            int tc_size;
            int j;

            if (!item || !blocks) continue;

            if (cJSON_IsString(content) && content->valuestring[0])
            {
                cJSON *text = cJSON_CreateObject();

                if (text)
                {
                    cJSON_AddStringToObject(text, "type", "text");
                    cJSON_AddStringToObject(text, "text", content->valuestring);
                    cJSON_AddItemToArray(blocks, text);
                }
            }

            tool_calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
            tc_size = cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0;
            for (j = 0; j < tc_size; j++)
            {
                cJSON *tool_call = cJSON_GetArrayItem(tool_calls, j);
                cJSON *block;

                if (!cJSON_IsObject(tool_call)) continue;
                block = anthropic_tool_use_block(tool_call);
                if (block) cJSON_AddItemToArray(blocks, block);
            }

            if (cJSON_GetArraySize(blocks) == 0)
            {
                cJSON_Delete(blocks);
                cJSON_Delete(item);
                continue;
            }
            cJSON_AddStringToObject(item, "role", "assistant");
            cJSON_AddItemToObject(item, "content", blocks);
            cJSON_AddItemToArray(out, item);
        }
        else if (rt_strcmp(role, "tool") == 0)
        {
            const char *tool_use_id = get_string(msg, "tool_call_id");
            const char *text = cJSON_IsString(content) ? content->valuestring : RT_NULL;
            cJSON *block = cJSON_CreateObject();

            if (!block) continue;

            cJSON_AddStringToObject(block, "type", "tool_result");
            cJSON_AddStringToObject(block, "tool_use_id", tool_use_id ? tool_use_id : "");
            cJSON_AddStringToObject(block, "content", text ? text : "");

            /* 连续的 tool 结果要合并进同一条 user 消息，Anthropic 不接受连续同角色消息 */
            if (last_is_tool_result_message(out))
            {
                cJSON *last = cJSON_GetArrayItem(out, cJSON_GetArraySize(out) - 1);
                cJSON *blocks_old = cJSON_GetObjectItemCaseSensitive(last, "content");

                cJSON_AddItemToArray(blocks_old, block);
            }
            else
            {
                cJSON *blocks = cJSON_CreateArray();
                cJSON *message = cJSON_CreateObject();

                if (!blocks || !message)
                {
                    if (blocks) cJSON_Delete(blocks);
                    if (message) cJSON_Delete(message);
                    cJSON_Delete(block);
                    continue;
                }
                cJSON_AddItemToArray(blocks, block);
                cJSON_AddStringToObject(message, "role", "user");
                cJSON_AddItemToObject(message, "content", blocks);
                cJSON_AddItemToArray(out, message);
            }
        }
    }

    if (system_buf)
    {
        *system_out = system_buf;
    }

    return out;
}

/*
 * @brief 把 OpenAI 风格的工具定义转成 Anthropic 的工具定义
 * @param tools OpenAI 风格工具数组
 * @return Anthropic 工具数组
 */
static cJSON *anthropic_translate_tools(cJSON *tools)
{
    cJSON *out = cJSON_CreateArray();
    int size;
    int i;

    if (!out) return RT_NULL;
    if (!tools || !cJSON_IsArray(tools))
    {
        return out;
    }

    size = cJSON_GetArraySize(tools);
    for (i = 0; i < size; i++)
    {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *func = cJSON_GetObjectItemCaseSensitive(tool, "function");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(func, "parameters");
        const char *name;
        const char *description;
        cJSON *item;

        if (!cJSON_IsObject(tool)) continue;

        name = get_string(func, "name");
        description = get_string(func, "description");
        item = cJSON_CreateObject();
        if (!item) continue;

        cJSON_AddStringToObject(item, "name", name ? name : "");
        if (description)
        {
            cJSON_AddStringToObject(item, "description", description);
        }
        if (params && cJSON_IsObject(params))
        {
            cJSON_AddItemToObject(item, "input_schema", cJSON_Duplicate(params, cJSON_True));
        }
        else
        {
            cJSON *schema = cJSON_CreateObject();
            cJSON *properties = cJSON_CreateObject();

            if (schema && properties)
            {
                cJSON_AddStringToObject(schema, "type", "object");
                cJSON_AddItemToObject(schema, "properties", properties);
                cJSON_AddItemToObject(item, "input_schema", schema);
            }
            else
            {
                if (schema) cJSON_Delete(schema);
                if (properties) cJSON_Delete(properties);
            }
        }
        cJSON_AddItemToArray(out, item);
    }

    return out;
}

/*
 * @brief 拼 Anthropic 请求体
 * @param payload    请求体对象
 * @param messages   规范格式的消息数组
 * @param tools      OpenAI 风格工具数组
 * @param max_tokens 最大输出 token
 * @return RT_EOK 成功
 */
static rt_err_t anthropic_build_request(cJSON *payload, cJSON *messages, cJSON *tools, int max_tokens)
{
    char *system_text = RT_NULL;
    cJSON *msg_array;
    cJSON *tool_array;
    int limit = max_tokens;

    if (!payload || !messages)
    {
        return -RT_EINVAL;
    }

    /* 模型对 max_tokens 有自己的上限，超了直接被拒，这里收一下 */
    if (limit > PKG_AGENT_ANTHROPIC_MAX_TOKENS)
    {
        limit = PKG_AGENT_ANTHROPIC_MAX_TOKENS;
    }
    if (limit < 1)
    {
        limit = 1;
    }

    if (!cJSON_AddStringToObject(payload, "model", get_dynamic_agent_model_name()))
    {
        return -RT_ENOMEM;
    }
    if (!cJSON_AddNumberToObject(payload, "max_tokens", limit))
    {
        return -RT_ENOMEM;
    }
    if (!cJSON_AddBoolToObject(payload, "stream", RT_TRUE))
    {
        return -RT_ENOMEM;
    }

    msg_array = anthropic_translate_messages(messages, &system_text);
    if (!msg_array)
    {
        return -RT_ENOMEM;
    }
    if (system_text)
    {
        cJSON_AddStringToObject(payload, "system", system_text);
        rt_free(system_text);
    }
    if (!cJSON_AddItemToObject(payload, "messages", msg_array))
    {
        cJSON_Delete(msg_array);
        return -RT_ENOMEM;
    }

    if (tools != RT_NULL)
    {
        tool_array = anthropic_translate_tools(tools);
        if (!tool_array)
        {
            return -RT_ENOMEM;
        }
        if (!cJSON_AddItemToObject(payload, "tools", tool_array))
        {
            cJSON_Delete(tool_array);
            return -RT_ENOMEM;
        }
    }

    return RT_EOK;
}

/*
 * @brief 加 Anthropic 的鉴权头与版本头
 * @param session webclient 会话
 */
static void anthropic_add_headers(struct webclient_session *session)
{
    char key_header[256] = {0};
    char version_header[64] = {0};

    rt_snprintf(key_header, sizeof(key_header),
                "x-api-key: %s\r\n", get_dynamic_agent_api_key());
    rt_snprintf(version_header, sizeof(version_header),
                "anthropic-version: %s\r\n", PKG_AGENT_ANTHROPIC_VERSION);

    webclient_header_fields_add(session, key_header);
    webclient_header_fields_add(session, version_header);
}

/*
 * @brief 解析一条 Anthropic SSE 事件
 * @param json   事件 JSON
 * @param stream 流状态
 * @return RT_TRUE 表示流已结束
 */
static rt_bool_t anthropic_parse_event(cJSON *json, agent_protocol_stream_t *stream)
{
    const char *type;
    cJSON *index_node;

    if (!json || !stream)
    {
        return RT_FALSE;
    }

    type = get_string(json, "type");
    if (!type)
    {
        stream->content_lost = RT_TRUE;
        return RT_FALSE;
    }

    index_node = cJSON_GetObjectItemCaseSensitive(json, "index");
    {
        int index = (index_node && cJSON_IsNumber(index_node)) ? index_node->valueint : 0;

        if (rt_strcmp(type, "content_block_start") == 0)
        {
            cJSON *block = cJSON_GetObjectItemCaseSensitive(json, "content_block");
            const char *block_type = get_string(block, "type");

            if (block_type && rt_strcmp(block_type, "tool_use") == 0)
            {
                stream->saw_tool_call = RT_TRUE;
                agent_protocol_tool_begin(stream, index,
                                          get_string(block, "id"),
                                          get_string(block, "name"));
            }
            else if (block_type && rt_strcmp(block_type, "text") == 0)
            {
                agent_protocol_add_content(stream, get_string(block, "text"));
            }
            else if (block_type && rt_strcmp(block_type, "thinking") == 0)
            {
                agent_protocol_add_reasoning(stream, get_string(block, "thinking"));
            }
        }
        else if (rt_strcmp(type, "content_block_delta") == 0)
        {
            cJSON *delta = cJSON_GetObjectItemCaseSensitive(json, "delta");
            const char *delta_type = get_string(delta, "type");

            if (delta_type && rt_strcmp(delta_type, "text_delta") == 0)
            {
                agent_protocol_add_content(stream, get_string(delta, "text"));
            }
            else if (delta_type && rt_strcmp(delta_type, "thinking_delta") == 0)
            {
                agent_protocol_add_reasoning(stream, get_string(delta, "thinking"));
            }
            else if (delta_type && rt_strcmp(delta_type, "input_json_delta") == 0)
            {
                agent_protocol_tool_args(stream, index, get_string(delta, "partial_json"));
            }
        }
        else if (rt_strcmp(type, "message_delta") == 0)
        {
            cJSON *delta = cJSON_GetObjectItemCaseSensitive(json, "delta");
            const char *stop_reason = get_string(delta, "stop_reason");

            if (stop_reason)
            {
                if (rt_strcmp(stop_reason, "end_turn") == 0)
                {
                    stream->natural_stop = RT_TRUE;
                }
                else if (rt_strcmp(stop_reason, "tool_use") == 0)
                {
                    stream->saw_tool_call = RT_TRUE;
                }
                else
                {
                    /* max_tokens / stop_sequence / refusal 等都不算完整收尾 */
                    stream->content_lost = RT_TRUE;
                }
            }
        }
        else if (rt_strcmp(type, "message_stop") == 0)
        {
            stream->stream_done = RT_TRUE;
            return RT_TRUE;
        }
        else if (rt_strcmp(type, "error") == 0)
        {
            LOG_E("anthropic: error event received");
            stream->content_lost = RT_TRUE;
        }
    }

    return RT_FALSE;
}

static const agent_protocol_ops_t g_anthropic_ops =
{
    .name = "anthropic",
    .build_request = anthropic_build_request,
    .add_headers = anthropic_add_headers,
    .parse_event = anthropic_parse_event,
    .format_content = anthropic_format_content,
};

/*
 * @brief 取 Anthropic 协议的操作表
 * @return 操作表指针
 */
const agent_protocol_ops_t *agent_protocol_anthropic_ops(void)
{
    return &g_anthropic_ops;
}
