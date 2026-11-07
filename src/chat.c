#include "chat.h"

#define LOG_TAG "Agent.chat"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define POST_RESP_BUFSZ    PKG_AGENT_RESP_BUFSZ
#define POST_HEADER_BUFSZ  PKG_AGENT_WEB_SOCKET_BUFSZ
#define STREAM_LINE_BUFSZ  PKG_AGENT_STREAM_LINE_BUFSZ

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
 *
 * 报文格式由当前协议决定（OpenAI 或 Anthropic），见 utils/protocol.h。
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
    const agent_protocol_ops_t *ops = agent_protocol_get_ops();
    struct webclient_session *webSession = RT_NULL;
    unsigned char *buffer = RT_NULL;
    cJSON *payload_json = RT_NULL;
    char *payload = RT_NULL;
    ChatResponse_t response = RT_NULL;
    agent_protocol_stream_t stream;

    int bytes_read, resp_status;
    rt_bool_t skip_line = RT_FALSE;
    rt_bool_t stream_done = RT_FALSE;
    char stream_buffer[STREAM_LINE_BUFSZ] = {0};
    int stream_len = 0;

    agent_protocol_stream_init(&stream, on_reasoning, on_tool_call, on_context);

    /* 1. 分配网络接收缓冲区 */
    buffer = (unsigned char *)web_malloc(POST_RESP_BUFSZ);
    if (buffer == RT_NULL)
    {
        LOG_E("chat: no memory for response buffer.\n");
        goto __cleanup;
    }

    /* 2. 创建 webclient 会话 */
    webSession = webclient_session_create(POST_HEADER_BUFSZ);
    if (webSession == RT_NULL)
    {
        LOG_E("chat: webclient session create failed\n");
        goto __cleanup;
    }
    /* 设置超时，避免网络卡死导致清理流程无限等待 */
    webclient_set_timeout(webSession, 30000);

    /* 3. 按协议构造请求体 */
    payload_json = cJSON_CreateObject();
    if (!payload_json)
    {
        goto __cleanup;
    }
    if (ops->build_request(payload_json, messages, tools, max_tokens) != RT_EOK)
    {
        LOG_E("chat: build request fail\n");
        goto __cleanup;
    }

    payload = cJSON_PrintUnformatted(payload_json);
    cJSON_Delete(payload_json);
    payload_json = RT_NULL;
    if (!payload)
    {
        LOG_E("chat: payload json print malloc fail\n");
        goto __cleanup;
    }
    int payload_len = rt_strlen(payload);

    /* 4. 设置请求头 */
    webclient_header_fields_add(webSession, "Content-Type: application/json\r\n");
    ops->add_headers(webSession);
    webclient_header_fields_add(webSession, "Content-Length: %d\r\n", payload_len);

    /* 5. 发送 POST 请求 */
    resp_status = webclient_post(webSession, get_dynamic_agent_api_url(), payload, payload_len);
    if (resp_status != 200)
    {
        LOG_E("chat: POST failed, http status=%d\n", resp_status);
        goto __cleanup;
    }

    /* 6. 流式读取 SSE 数据 */
    while ((bytes_read = webclient_read(webSession, buffer, POST_RESP_BUFSZ)) > 0)
    {
        for (int i = 0; i < bytes_read; i++)
        {
            unsigned char ch = buffer[i];
            if (ch == '\n')
            {
                if (skip_line)
                {
                    /* 上一条 SSE 行超长已丢弃，本条是它的剩余部分，直接跳过 */
                    skip_line = RT_FALSE;
                    stream_len = 0;
                    rt_memset(stream_buffer, 0, sizeof(stream_buffer));
                    continue;
                }
                if (stream_len > 0 && stream_buffer[stream_len - 1] == '\r')
                    stream_len--;
                stream_buffer[stream_len] = '\0';
                if (rt_strncmp(stream_buffer, "data:", 5) == 0)
                {
                    char *json_str = stream_buffer + 5;
                    if (*json_str == ' ') json_str++;
                    /* OpenAI 用 [DONE] 收尾，Anthropic 用 message_stop 事件 */
                    if (rt_strcmp(json_str, "[DONE]") == 0)
                    {
                        stream_done = RT_TRUE;
                        goto __success;
                    }

                    cJSON *json = cJSON_ParseWithOpts(json_str, RT_NULL, cJSON_True);
                    if (!json)
                    {
                        stream.content_lost = RT_TRUE;
                        stream_len = 0;
                        rt_memset(stream_buffer, 0, sizeof(stream_buffer));
                        continue;
                    }

                    if (ops->parse_event(json, &stream))
                    {
                        cJSON_Delete(json);
                        stream_done = RT_TRUE;
                        goto __success;
                    }
                    cJSON_Delete(json);
                }
                stream_len = 0;
                rt_memset(stream_buffer, 0, sizeof(stream_buffer));
            }
            else
            {
                if (skip_line)
                {
                    continue;
                }
                if (stream_len < STREAM_LINE_BUFSZ - 1)
                {
                    stream_buffer[stream_len++] = ch;
                }
                else
                {
                    /* SSE 行超长：丢弃整行并告警（含 tool_calls 的报文行约 300+ 字节，
                       默认 1KB 足够；如遇超长参数分片请调大 PKG_AGENT_STREAM_LINE_BUFSZ） */
                    LOG_W("chat: SSE line exceeds %d bytes, discarded; "
                          "raise PKG_AGENT_STREAM_LINE_BUFSZ if tool calls are missing\n",
                          STREAM_LINE_BUFSZ);
                    skip_line = RT_TRUE;
                    stream.content_lost = RT_TRUE;
                    stream_len = 0;
                    rt_memset(stream_buffer, 0, sizeof(stream_buffer));
                }
            }
        }
    }

__success:
    stream.stream_done = stream.stream_done || stream_done;

    /* 把分片归一化成 tool_calls，并回调 on_tool_call */
    agent_protocol_tool_finalize(&stream);

    response = (ChatResponse_t)rt_malloc(sizeof(ChatResponse));
    if (response == RT_NULL)
    {
        LOG_E("Failed malloc response");
        goto __cleanup;
    }
    response->context = stream.content;
    response->reasoning = stream.reasoning;
    response->tool_call = stream.tool_calls;
    response->complete = stream.stream_done && stream.natural_stop &&
                         !stream.content_lost && !stream.saw_tool_call;
    /* 响应接管这三个缓冲区，释放流状态时别再释放一次 */
    stream.content = RT_NULL;
    stream.reasoning = RT_NULL;
    stream.tool_calls = RT_NULL;

__cleanup:
    cJSON_Delete(payload_json);
    if (webSession)
        webclient_close(webSession);
    if (buffer)
        web_free(buffer);
    if (payload)
        cJSON_free(payload);

    agent_protocol_stream_deinit(&stream);

    return response;
}
