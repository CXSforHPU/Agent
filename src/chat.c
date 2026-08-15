#include "chat.h"

#define LOG_TAG "Agent.chat"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define POST_RESP_BUFSZ    PKG_AGENT_RESP_BUFSZ
#define POST_HEADER_BUFSZ  PKG_AGENT_WEB_SOCKET_BUFSZ
#define MAX_REASONING_LEN  PKG_AGENT_MAX_REASONING_LEN
#define MAX_CONTENT_LEN    PKG_AGENT_MAX_CONTENT_LEN
#define STREAM_LINE_BUFSZ  PKG_AGENT_STREAM_LINE_BUFSZ
#define MAX_TOOL_ARG_LEN   PKG_AGENT_MAX_TOOL_ARG_LEN

/*
 * @brief 释放聊天响应结构体
 * @param resp 要释放的响应
 */
void chat_response_free(ChatResponse_t resp)
{
    if (!resp) return;
    rt_free(resp->reasoning);
    rt_free(resp->context);
    cJSON_Delete(resp->tool_call);
    rt_free(resp);
}

/*
 * @brief 发送聊天请求并流式读取 SSE 响应
 * @param messages       消息列表 JSON 数组
 * @param tools          工具定义 JSON 数组
 * @param max_tokens     最大 token 数
 * @param on_reasoning   思考过程回调
 * @param on_tool_call   工具调用回调
 * @param on_context     回复内容回调
 * @return ChatResponse_t 响应结构体，失败返回 NULL
 */
ChatResponse_t chat(
    cJSON *messages,
    cJSON *tools,
    int max_tokens,
    void (*on_reasoning)(const char *text),
    void (*on_tool_call)(const char *text),
    void (*on_context)(const char *text))
{
    struct webclient_session *webSession = RT_NULL;
    unsigned char *buffer = RT_NULL;
    char *reasoning = RT_NULL;
    char *content = RT_NULL;
    cJSON *tool_calls = RT_NULL;
    cJSON *tool_call_map = RT_NULL;
    char *payload = RT_NULL;
    ChatResponse_t response = RT_NULL;

    int bytes_read, resp_status;
    int reasoning_len = 0;
    int content_len = 0;
    rt_bool_t is_answering = RT_FALSE;
    char stream_buffer[STREAM_LINE_BUFSZ] = {0};
    int stream_len = 0;

    /* 1. 分配网络接收缓冲区 */
    buffer = (unsigned char *)web_malloc(POST_RESP_BUFSZ);
    if (buffer == RT_NULL)
    {
        LOG_E("chat: no memory for response buffer.\n");
        goto __cleanup;
    }

    /* 2. 分配文本缓存 */
    reasoning = (char *)rt_malloc(MAX_REASONING_LEN);
    content = (char *)rt_malloc(MAX_CONTENT_LEN);
    if (!reasoning || !content)
    {
        LOG_E("chat: Failed to allocate reasoning/content buf\n");
        goto __cleanup;
    }
    rt_memset(reasoning, 0, MAX_REASONING_LEN);
    rt_memset(content, 0, MAX_CONTENT_LEN);

    /* 3. 工具调用临时缓存 */
    tool_calls = cJSON_CreateArray();
    tool_call_map = cJSON_CreateObject();
    if (!tool_calls || !tool_call_map)
    {
        LOG_E("chat: create cJSON tool array/map fail\n");
        goto __cleanup;
    }

    /* 4. 创建 webclient 会话 */
    webSession = webclient_session_create(POST_HEADER_BUFSZ);
    if (webSession == RT_NULL)
    {
        LOG_E("chat: webclient session create failed\n");
        goto __cleanup;
    }
    /* 设置超时，避免网络卡死导致清理流程无限等待 */
    webclient_set_timeout(webSession, 30000);

    /* 5. 构造请求 JSON */
    cJSON *payload_json = cJSON_CreateObject();
    cJSON_AddStringToObject(payload_json, "model", get_dynamic_agent_model_name());
    cJSON_AddItemToObject(payload_json, "messages", cJSON_Duplicate(messages, cJSON_True));
    if (tools != RT_NULL)
    {
        cJSON_AddItemToObject(payload_json, "tools", cJSON_Duplicate(tools, cJSON_True));
    }
    cJSON_AddNumberToObject(payload_json, "max_tokens", max_tokens);
    cJSON_AddBoolToObject(payload_json, "stream", RT_TRUE);

    payload = cJSON_PrintUnformatted(payload_json);
    cJSON_Delete(payload_json);
    if (!payload)
    {
        LOG_E("chat: payload json print malloc fail\n");
        goto __cleanup;
    }
    int payload_len = rt_strlen(payload);

    /* 6. 设置请求头 */
    char authHeader[256] = {0};
    rt_snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s\r\n", get_dynamic_agent_api_key());
    webclient_header_fields_add(webSession, "Content-Type: application/json\r\n");
    webclient_header_fields_add(webSession, authHeader);
    webclient_header_fields_add(webSession, "Content-Length: %d\r\n", payload_len);

    /* 7. 发送 POST 请求 */
    resp_status = webclient_post(webSession, get_dynamic_agent_api_url(), payload, payload_len);
    if (resp_status != 200)
    {
        LOG_E("chat: POST failed, http status=%d\n", resp_status);
        goto __cleanup;
    }

    /* 8. 流式读取 SSE 数据 */
    while ((bytes_read = webclient_read(webSession, buffer, POST_RESP_BUFSZ)) > 0)
    {
        for (int i = 0; i < bytes_read; i++)
        {
            unsigned char ch = buffer[i];
            if (ch == '\n')
            {
                stream_buffer[stream_len] = '\0';
                if (rt_strncmp(stream_buffer, "data: ", 6) == 0)
                {
                    char *json_str = stream_buffer + 6;
                    if (rt_strcmp(json_str, "[DONE]") == 0)
                    {
                        goto __success;
                    }

                    cJSON *json = cJSON_Parse(json_str);
                    if (!json)
                    {
                        /* 解析失败：复位行缓冲，避免残留内容污染下一条 SSE 行 */
                        stream_len = 0;
                        rt_memset(stream_buffer, 0, sizeof(stream_buffer));
                        continue;
                    }

                    cJSON *choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
                    if (choices && cJSON_IsArray(choices))
                    {
                        cJSON *choice = cJSON_GetArrayItem(choices, 0);
                        if (choice && cJSON_IsObject(choice))
                        {
                            cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
                            if (delta && cJSON_IsObject(delta))
                            {
                                /* 解析思考过程 */
                                cJSON *reasoning_field = cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
                                if (reasoning_field && cJSON_IsString(reasoning_field))
                                {
                                    const char *seg = reasoning_field->valuestring;
                                    size_t seg_len = rt_strlen(seg);
                                    int remain = MAX_REASONING_LEN - reasoning_len - 1;
                                    if (remain > 0)
                                    {
                                        if ((int)seg_len > remain) seg_len = remain;
                                        rt_strncpy(reasoning + reasoning_len, seg, seg_len);
                                        reasoning_len += seg_len;
                                        reasoning[reasoning_len] = '\0';
                                        if (on_reasoning)
                                            on_reasoning(seg);
                                    }
                                }

                                /* 解析回答正文 */
                                cJSON *content_field = cJSON_GetObjectItemCaseSensitive(delta, "content");
                                if (content_field && cJSON_IsString(content_field))
                                {
                                    const char *seg = content_field->valuestring;
                                    size_t seg_len = rt_strlen(seg);
                                    if (!is_answering)
                                    {
                                        rt_kprintf("\n");
                                        is_answering = RT_TRUE;
                                    }
                                    int remain = MAX_CONTENT_LEN - content_len - 1;
                                    if (remain > 0)
                                    {
                                        if ((int)seg_len > remain) seg_len = remain;
                                        rt_strncpy(content + content_len, seg, seg_len);
                                        content_len += seg_len;
                                        content[content_len] = '\0';
                                        if (on_context)
                                            on_context(seg);
                                    }
                                }

                                /* 流式拼接 tool_calls */
                                cJSON *tc_arr = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
                                if (tc_arr && cJSON_IsArray(tc_arr))
                                {
                                    int arr_sz = cJSON_GetArraySize(tc_arr);
                                    for (int j = 0; j < arr_sz; j++)
                                    {
                                        cJSON *tc_item = cJSON_GetArrayItem(tc_arr, j);
                                        if (!tc_item || !cJSON_IsObject(tc_item))
                                            continue;

                                        cJSON *id_node = cJSON_GetObjectItemCaseSensitive(tc_item, "id");
                                        cJSON *func_node = cJSON_GetObjectItemCaseSensitive(tc_item, "function");
                                        cJSON *idx_node = cJSON_GetObjectItemCaseSensitive(tc_item, "index");
                                        cJSON *type_node = cJSON_GetObjectItemCaseSensitive(tc_item, "type");
                                        if (!id_node || !func_node || !idx_node || !type_node)
                                            continue;

                                        char key_buf[32] = {0};
                                        rt_snprintf(key_buf, sizeof(key_buf), "%d", idx_node->valueint);
                                        cJSON *map_entry = cJSON_GetObjectItemCaseSensitive(tool_call_map, key_buf);
                                        if (!map_entry)
                                        {
                                            map_entry = cJSON_CreateObject();
                                            cJSON_AddStringToObject(map_entry, "id", id_node->valuestring);
                                            cJSON_AddNumberToObject(map_entry, "index", idx_node->valueint);
                                            cJSON_AddStringToObject(map_entry, "type", type_node->valuestring);
                                            cJSON_AddItemToObject(map_entry, "function", cJSON_CreateObject());
                                            cJSON_AddItemToObject(tool_call_map, key_buf, map_entry);
                                        }
                                        cJSON *map_func = cJSON_GetObjectItemCaseSensitive(map_entry, "function");
                                        if (!map_func) continue;

                                        /* 填充函数名 */
                                        cJSON *name_node = cJSON_GetObjectItemCaseSensitive(func_node, "name");
                                        if (name_node && cJSON_IsString(name_node))
                                        {
                                            cJSON *exist_name = cJSON_GetObjectItemCaseSensitive(map_func, "name");
                                            if (!exist_name)
                                            {
                                                cJSON_AddStringToObject(map_func, "name", name_node->valuestring);
                                            }
                                        }

                                        /* 拼接 arguments 分片 */
                                        cJSON *args_node = cJSON_GetObjectItemCaseSensitive(func_node, "arguments");
                                        if (args_node && cJSON_IsString(args_node))
                                        {
                                            const char *append_str = args_node->valuestring;
                                            size_t append_len = rt_strlen(append_str);
                                            if (append_len == 0)
                                                continue;

                                            cJSON *exist_args = cJSON_GetObjectItemCaseSensitive(map_func, "arguments");
                                            if (!exist_args)
                                            {
                                                cJSON_AddStringToObject(map_func, "arguments", append_str);
                                            }
                                            else
                                            {
                                                const char *old_str = exist_args->valuestring;
                                                size_t old_len = rt_strlen(old_str);
                                                size_t total_len = old_len + append_len;

                                                if (total_len >= MAX_TOOL_ARG_LEN)
                                                {
                                                    LOG_W("tool arguments exceed MAX_TOOL_ARG_LEN, skip append\n");
                                                    continue;
                                                }

                                                char *combine_buf = rt_malloc(total_len + 1);
                                                if (combine_buf == RT_NULL)
                                                {
                                                    LOG_E("tool args combine malloc fail\n");
                                                    continue;
                                                }
                                                rt_memcpy(combine_buf, old_str, old_len);
                                                rt_memcpy(combine_buf + old_len, append_str, append_len);
                                                combine_buf[total_len] = '\0';

                                                cJSON_DetachItemFromObject(map_func, "arguments");
                                                cJSON_Delete(exist_args);
                                                cJSON_AddStringToObject(map_func, "arguments", combine_buf);
                                                rt_free(combine_buf);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    cJSON_Delete(json);
                }
                stream_len = 0;
                rt_memset(stream_buffer, 0, sizeof(stream_buffer));
            }
            else
            {
                if (stream_len < STREAM_LINE_BUFSZ - 1)
                {
                    stream_buffer[stream_len++] = ch;
                }
                else
                {
                    stream_len = 0;
                    rt_memset(stream_buffer, 0, sizeof(stream_buffer));
                }
            }
        }
    }

__success:
    reasoning[reasoning_len] = '\0';
    content[content_len] = '\0';

    /* 将 map 中的 tool_call 转移到最终数组 */
    cJSON *map_key = NULL;
    cJSON *map_val = NULL;
    for (map_key = tool_call_map->child; map_key != NULL; map_key = map_key->next)
    {
        map_val = map_key;
        cJSON *dup_item = cJSON_Duplicate(map_val, cJSON_True);
        cJSON_AddItemToArray(tool_calls, dup_item);
    }

    /* 回调完整 tool_call */
    if (on_tool_call != RT_NULL)
    {
        int tool_cnt = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tool_cnt; i++)
        {
            cJSON *tool_item = cJSON_GetArrayItem(tool_calls, i);
            char *tool_json = cJSON_PrintUnformatted(tool_item);
            if (tool_json)
            {
                on_tool_call(tool_json);
                cJSON_free(tool_json);
            }
        }
    }

    response = (ChatResponse_t)rt_malloc(sizeof(ChatResponse));
    if (response == RT_NULL)
    {
        LOG_E("Failed malloc response");
        goto __cleanup;
    }
    response->context = content;
    response->reasoning = reasoning;
    response->tool_call = tool_calls;
    goto __cleanup;

__cleanup:
    if (webSession)
        webclient_close(webSession);
    if (buffer)
        web_free(buffer);
    if (payload)
        cJSON_free(payload);

    if (response == RT_NULL)
    {
        if (reasoning) rt_free(reasoning);
        if (content) rt_free(content);
        if (tool_calls) cJSON_Delete(tool_calls);
    }

    if (tool_call_map)
        cJSON_Delete(tool_call_map);

    return response;
}