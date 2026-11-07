#include "utils/utils_internal.h"

#define LOG_TAG "Agent.protocol.openai"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * OpenAI 协议：POST /chat/completions
 * 请求体 {model, messages, tools, max_tokens, stream}，鉴权用 Bearer 头，
 * 响应是 delta 式 SSE：data: {"choices":[{"delta":{...}}]}，以 data: [DONE] 收尾。
 */

/*
 * @brief 把规范格式的 content 转成 OpenAI 的 content
 * @param content 规范格式的 content，字符串或块数组
 * @return content 副本
 */
static cJSON *openai_format_content(cJSON *content)
{
    if (!content)
    {
        return cJSON_CreateString("");
    }
    return cJSON_Duplicate(content, cJSON_True);
}

/*
 * @brief 拼 OpenAI 请求体
 * @param payload    请求体对象
 * @param messages   消息数组
 * @param tools      工具定义数组
 * @param max_tokens 最大输出 token
 * @return RT_EOK 成功
 */
static rt_err_t openai_build_request(cJSON *payload, cJSON *messages, cJSON *tools, int max_tokens)
{
    cJSON *copy;

    if (!payload || !messages)
    {
        return -RT_EINVAL;
    }

    if (!cJSON_AddStringToObject(payload, "model", get_dynamic_agent_model_name()))
    {
        return -RT_ENOMEM;
    }

    copy = cJSON_Duplicate(messages, cJSON_True);
    if (!copy || !cJSON_AddItemToObject(payload, "messages", copy))
    {
        cJSON_Delete(copy);
        return -RT_ENOMEM;
    }

    if (tools != RT_NULL)
    {
        copy = cJSON_Duplicate(tools, cJSON_True);
        if (!copy || !cJSON_AddItemToObject(payload, "tools", copy))
        {
            cJSON_Delete(copy);
            return -RT_ENOMEM;
        }
    }

    if (!cJSON_AddNumberToObject(payload, "max_tokens", max_tokens) ||
        !cJSON_AddBoolToObject(payload, "stream", RT_TRUE))
    {
        return -RT_ENOMEM;
    }

    return RT_EOK;
}

/*
 * @brief 加 OpenAI 的鉴权头
 * @param session webclient 会话
 */
static void openai_add_headers(struct webclient_session *session)
{
    char auth_header[256] = {0};

    rt_snprintf(auth_header, sizeof(auth_header),
                "Authorization: Bearer %s\r\n", get_dynamic_agent_api_key());
    webclient_header_fields_add(session, auth_header);
}

/*
 * @brief 解析一条 OpenAI SSE 事件
 * @param json   事件 JSON
 * @param stream 流状态
 * @return 恒为 RT_FALSE，OpenAI 用 data: [DONE] 收尾
 */
static rt_bool_t openai_parse_event(cJSON *json, agent_protocol_stream_t *stream)
{
    cJSON *choices;
    cJSON *choice;

    if (!json || !stream)
    {
        return RT_FALSE;
    }

    choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetObjectItemCaseSensitive(json, "error"))
    {
        stream->content_lost = RT_TRUE;
    }
    if (!choices || !cJSON_IsArray(choices))
    {
        return RT_FALSE;
    }

    choice = cJSON_GetArrayItem(choices, 0);
    if (choice && !cJSON_IsObject(choice))
    {
        stream->content_lost = RT_TRUE;
    }
    if (!choice || !cJSON_IsObject(choice))
    {
        return RT_FALSE;
    }

    {
        cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");

        if (cJSON_IsString(finish))
        {
            stream->natural_stop = (rt_strcmp(finish->valuestring, "stop") == 0);
            if (!stream->natural_stop) stream->content_lost = RT_TRUE;
        }
        if (!cJSON_IsObject(delta) && !cJSON_IsString(finish))
        {
            stream->content_lost = RT_TRUE;
        }

        if (delta && cJSON_IsObject(delta))
        {
            /* 思考过程：兼容 reasoning_content（OpenAI 兼容推理模型）与 reasoning 两种字段名 */
            cJSON *reasoning_field = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");

            if (!reasoning_field || !cJSON_IsString(reasoning_field))
            {
                reasoning_field = cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
            }
            if (reasoning_field && cJSON_IsString(reasoning_field))
            {
                agent_protocol_add_reasoning(stream, reasoning_field->valuestring);
            }

            /* 回答正文，空分片不产生输出 */
            {
                cJSON *content_field = cJSON_GetObjectItemCaseSensitive(delta, "content");

                if (content_field && !cJSON_IsNull(content_field) && !cJSON_IsString(content_field))
                {
                    stream->content_lost = RT_TRUE;
                }
                if (content_field && cJSON_IsString(content_field))
                {
                    agent_protocol_add_content(stream, content_field->valuestring);
                }
            }

            /* 流式拼接 tool_calls */
            {
                cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");

                if (tc_arr && !cJSON_IsNull(tc_arr) &&
                    (!cJSON_IsArray(tc_arr) || cJSON_GetArraySize(tc_arr) > 0))
                {
                    stream->saw_tool_call = RT_TRUE;
                }
                if (tc_arr && cJSON_IsArray(tc_arr))
                {
                    int arr_sz = cJSON_GetArraySize(tc_arr);

                    for (int j = 0; j < arr_sz; j++)
                    {
                        cJSON *tc_item = cJSON_GetArrayItem(tc_arr, j);
                        cJSON *id_node;
                        cJSON *func_node;
                        cJSON *idx_node;
                        int idx;

                        if (!tc_item || !cJSON_IsObject(tc_item))
                        {
                            continue;
                        }

                        id_node = cJSON_GetObjectItemCaseSensitive(tc_item, "id");
                        func_node = cJSON_GetObjectItemCaseSensitive(tc_item, "function");
                        idx_node = cJSON_GetObjectItemCaseSensitive(tc_item, "index");

                        /* 分片容错：只有首个分片带 id/type，后续分片这些字段可能为 null 或缺失 */
                        idx = (idx_node && cJSON_IsNumber(idx_node)) ? idx_node->valueint : 0;

                        agent_protocol_tool_begin(stream, idx,
                                                  (id_node && cJSON_IsString(id_node)) ? id_node->valuestring : RT_NULL,
                                                  RT_NULL);

                        if (!func_node || !cJSON_IsObject(func_node))
                        {
                            continue;
                        }

                        {
                            cJSON *name_node = cJSON_GetObjectItemCaseSensitive(func_node, "name");
                            cJSON *args_node = cJSON_GetObjectItemCaseSensitive(func_node, "arguments");

                            if (name_node && cJSON_IsString(name_node))
                            {
                                agent_protocol_tool_begin(stream, idx, RT_NULL, name_node->valuestring);
                            }
                            if (args_node && cJSON_IsString(args_node))
                            {
                                agent_protocol_tool_args(stream, idx, args_node->valuestring);
                            }
                        }
                    }
                }
            }
        }
    }

    return RT_FALSE;
}

static const agent_protocol_ops_t g_openai_ops =
{
    .name = "openai",
    .build_request = openai_build_request,
    .add_headers = openai_add_headers,
    .parse_event = openai_parse_event,
    .format_content = openai_format_content,
};

/*
 * @brief 取 OpenAI 协议的操作表
 * @return 操作表指针
 */
const agent_protocol_ops_t *agent_protocol_openai_ops(void)
{
    return &g_openai_ops;
}
