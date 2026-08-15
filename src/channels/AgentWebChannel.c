#include "AgentWebChannel.h"

#define CONFIG_REQUEST_MAX_SIZE 2048
#define CHAT_REQUEST_MAX_SIZE   4096

static MessageHub_t message_hub = RT_NULL;
static Context_t context = RT_NULL;
static rt_bool_t webnet_started = RT_FALSE;

/*
 * @brief 安全获取字符串（NULL 时返回空串）
 */
static const char *safe_string(const char *value)
{
    return value != RT_NULL ? value : "";
}

/*
 * @brief 发送原始 JSON 响应
 */
static void send_raw_json(struct webnet_session *session,
                          int status_code,
                          const char *reason,
                          const char *body)
{
    rt_size_t body_len;

    if (session == RT_NULL || session->request == RT_NULL || body == RT_NULL)
    {
        return;
    }

    body_len = strlen(body);
    session->request->result_code = status_code;
    webnet_session_set_header(session, "application/json", status_code,
                              reason, body_len);
    webnet_session_write(session, (const rt_uint8_t *)body, body_len);
}

/*
 * @brief 发送 cJSON 对象作为响应
 */
static rt_err_t send_json_object(struct webnet_session *session,
                                 int status_code,
                                 const char *reason,
                                 cJSON *json)
{
    char *body;

    if (session == RT_NULL || session->request == RT_NULL || json == RT_NULL)
    {
        return -RT_ERROR;
    }

    body = cJSON_PrintUnformatted(json);
    if (body == RT_NULL)
    {
        return -RT_ERROR;
    }

    send_raw_json(session, status_code, reason, body);
    cJSON_free(body);
    return RT_EOK;
}

/*
 * @brief 发送简单 JSON 响应 { success, [field]: message }
 */
static void send_simple_json(struct webnet_session *session,
                             int status_code,
                             const char *reason,
                             rt_bool_t success,
                             const char *field,
                             const char *message)
{
    cJSON *json = cJSON_CreateObject();

    if (json == RT_NULL)
    {
        send_raw_json(session, 500, "Internal Server Error",
                      "{\"success\":false,\"error\":\"Failed to build response\"}");
        return;
    }

    cJSON_AddBoolToObject(json, "success", success ? cJSON_True : cJSON_False);
    if (field != RT_NULL && message != RT_NULL)
    {
        cJSON_AddStringToObject(json, field, message);
    }

    if (send_json_object(session, status_code, reason, json) != RT_EOK)
    {
        send_raw_json(session, 500, "Internal Server Error",
                      "{\"success\":false,\"error\":\"Failed to serialize response\"}");
    }

    cJSON_Delete(json);
}

/*
 * @brief 复制请求体
 */
static char *copy_request_body(struct webnet_session *session,
                               rt_size_t max_size)
{
    char *body;
    rt_size_t body_len;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        return RT_NULL;
    }

    body_len = session->request->content_length;
    if (session->request->query == RT_NULL ||
        body_len == 0 ||
        body_len > max_size)
    {
        return RT_NULL;
    }

    body = rt_malloc(body_len + 1);
    if (body == RT_NULL)
    {
        return RT_NULL;
    }

    rt_memcpy(body, session->request->query, body_len);
    body[body_len] = '\0';
    return body;
}

/*
 * @brief 判断 JSON 值是否为 true
 */
static rt_bool_t json_value_is_true(const cJSON *item)
{
    const char *value;

    if (item == RT_NULL)
    {
        return RT_FALSE;
    }

    if (cJSON_IsBool(item))
    {
        return cJSON_IsTrue(item) ? RT_TRUE : RT_FALSE;
    }

    if (cJSON_IsNumber(item))
    {
        return item->valuedouble != 0 ? RT_TRUE : RT_FALSE;
    }

    if (!cJSON_IsString(item))
    {
        return RT_FALSE;
    }

    value = cJSON_GetStringValue(item);
    if (value == RT_NULL)
    {
        return RT_FALSE;
    }

    return strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "1") == 0;
}

/*
 * @brief SSE 流式发送事件
 */
static void agent_webnet_stream_send_event(struct webnet_session *session,
                                           const char *event,
                                           const char *data)
{
    const char *cursor;

    if (session == RT_NULL)
    {
        return;
    }

    if (event != RT_NULL && event[0] != '\0')
    {
        webnet_session_write(session, (const rt_uint8_t *)"event: ", 7);
        webnet_session_write(session, (const rt_uint8_t *)event, strlen(event));
        webnet_session_write(session, (const rt_uint8_t *)"\n", 1);
    }

    if (data == RT_NULL)
    {
        webnet_session_write(session, (const rt_uint8_t *)"data:\n\n", 7);
        return;
    }

    cursor = data;
    for (;;)
    {
        const char *newline = strchr(cursor, '\n');
        rt_size_t line_len = newline != RT_NULL
                           ? (rt_size_t)(newline - cursor)
                           : strlen(cursor);

        webnet_session_write(session, (const rt_uint8_t *)"data: ", 6);
        if (line_len > 0)
        {
            webnet_session_write(session, (const rt_uint8_t *)cursor, line_len);
        }
        webnet_session_write(session, (const rt_uint8_t *)"\n", 1);

        if (newline == RT_NULL)
        {
            break;
        }
        cursor = newline + 1;
    }

    webnet_session_write(session, (const rt_uint8_t *)"\n", 1);
}

/*
 * @brief 获取 LLM 配置（GET）
 */
static void cgi_get_config_handler(struct webnet_session *session)
{
    cJSON *json;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        LOG_E("get_config: invalid session");
        return;
    }

    json = cJSON_CreateObject();
    if (json == RT_NULL)
    {
        send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                         "error", "Failed to get configuration");
        return;
    }

    cJSON_AddBoolToObject(json, "success", cJSON_True);
    cJSON_AddStringToObject(json, "apiKey",
                            safe_string(get_dynamic_agent_api_key()));
    cJSON_AddStringToObject(json, "modelName",
                            safe_string(get_dynamic_agent_model_name()));
    cJSON_AddStringToObject(json, "apiUrl",
                            safe_string(get_dynamic_agent_api_url()));

    if (send_json_object(session, 200, "OK", json) != RT_EOK)
    {
        send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                         "error", "Failed to serialize configuration");
    }

    cJSON_Delete(json);
}

/*
 * @brief 更新 LLM 配置（POST）
 */
static void cgi_config_handler(struct webnet_session *session)
{
    char *body = RT_NULL;
    cJSON *request_json = RT_NULL;
    cJSON *item;
    const char *api_key;
    const char *model_name;
    const char *api_url;
    char *api_key_copy = RT_NULL;
    char *model_name_copy = RT_NULL;
    char *api_url_copy = RT_NULL;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        LOG_E("config: invalid session");
        return;
    }

    if (session->request->method != WEBNET_POST)
    {
        send_simple_json(session, 405, "Method Not Allowed", RT_FALSE,
                         "error", "Method not allowed (use POST)");
        return;
    }

    body = copy_request_body(session, CONFIG_REQUEST_MAX_SIZE);
    if (body == RT_NULL)
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "Invalid or oversized request body");
        return;
    }

    request_json = cJSON_Parse(body);
    if (request_json == RT_NULL || !cJSON_IsObject(request_json))
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "Invalid JSON configuration");
        goto cleanup;
    }

    api_key = safe_string(get_dynamic_agent_api_key());
    model_name = safe_string(get_dynamic_agent_model_name());
    api_url = safe_string(get_dynamic_agent_api_url());

    item = cJSON_GetObjectItem(request_json, "apiKey");
    if (item != RT_NULL)
    {
        if (!cJSON_IsString(item) ||
            cJSON_GetStringValue(item) == RT_NULL ||
            cJSON_GetStringValue(item)[0] == '\0')
        {
            send_simple_json(session, 400, "Bad Request", RT_FALSE,
                             "error", "apiKey must be a non-empty string");
            goto cleanup;
        }
        api_key = cJSON_GetStringValue(item);
    }

    item = cJSON_GetObjectItem(request_json, "modelName");
    if (item != RT_NULL)
    {
        if (!cJSON_IsString(item) ||
            cJSON_GetStringValue(item) == RT_NULL ||
            cJSON_GetStringValue(item)[0] == '\0')
        {
            send_simple_json(session, 400, "Bad Request", RT_FALSE,
                             "error", "modelName must be a non-empty string");
            goto cleanup;
        }
        model_name = cJSON_GetStringValue(item);
    }

    item = cJSON_GetObjectItem(request_json, "apiUrl");
    if (item != RT_NULL)
    {
        if (!cJSON_IsString(item) ||
            cJSON_GetStringValue(item) == RT_NULL ||
            cJSON_GetStringValue(item)[0] == '\0')
        {
            send_simple_json(session, 400, "Bad Request", RT_FALSE,
                             "error", "apiUrl must be a non-empty string");
            goto cleanup;
        }
        api_url = cJSON_GetStringValue(item);
    }

    if (api_key[0] == '\0')
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "API key is required");
        goto cleanup;
    }

    api_key_copy = rt_strdup(api_key);
    model_name_copy = rt_strdup(model_name);
    api_url_copy = rt_strdup(api_url);
    if (api_key_copy == RT_NULL ||
        model_name_copy == RT_NULL ||
        api_url_copy == RT_NULL)
    {
        send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                         "error", "Insufficient memory for configuration");
        goto cleanup;
    }

    agent_config_set(api_key_copy, model_name_copy, api_url_copy);

    if (context != RT_NULL)
    {
        if (context->clear_message != RT_NULL)
        {
            context->clear_message(context);
        }
        if (context->build_system_prompt != RT_NULL)
        {
            context->build_system_prompt(context);
        }
    }

    LOG_I("LLM configuration updated: MODEL=%s, URL=%s",
          safe_string(get_dynamic_agent_model_name()),
          safe_string(get_dynamic_agent_api_url()));

    send_simple_json(session, 200, "OK", RT_TRUE,
                     "message", "Configuration updated successfully");

cleanup:
    if (api_url_copy != RT_NULL)
        rt_free(api_url_copy);
    if (model_name_copy != RT_NULL)
        rt_free(model_name_copy);
    if (api_key_copy != RT_NULL)
        rt_free(api_key_copy);
    if (request_json != RT_NULL)
        cJSON_Delete(request_json);
    if (body != RT_NULL)
        rt_free(body);
}

/*
 * @brief 聊天处理（POST）
 */
static void cgi_chat_handler(struct webnet_session *session)
{
    char *body = RT_NULL;
    cJSON *request_json = RT_NULL;
    cJSON *item;
    char *user_message = RT_NULL;
    const char *ai_reply;
    Messages_t input_messages = messages_create(1);
    Messages_t output_message = RT_NULL;
    rt_bool_t stream_mode = RT_FALSE;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        LOG_E("chat: invalid session");
        return;
    }

    if (session->request->method != WEBNET_POST)
    {
        send_simple_json(session, 405, "Method Not Allowed", RT_FALSE,
                         "error", "Method not allowed (use POST)");
        return;
    }

    body = copy_request_body(session, CHAT_REQUEST_MAX_SIZE);
    if (body == RT_NULL)
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "Invalid or oversized request body");
        return;
    }

    request_json = cJSON_Parse(body);
    if (request_json == RT_NULL || !cJSON_IsObject(request_json))
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "Invalid JSON request");
        goto cleanup;
    }

    item = cJSON_GetObjectItem(request_json, "stream");
    stream_mode = json_value_is_true(item);

    item = cJSON_GetObjectItem(request_json, "reset");
    if (json_value_is_true(item))
    {
        if (context == RT_NULL ||
            context->clear_message == RT_NULL ||
            context->build_system_prompt == RT_NULL)
        {
            send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                             "error", "Conversation context is unavailable");
            goto cleanup;
        }

        context->clear_message(context);
        context->build_system_prompt(context);
        send_simple_json(session, 200, "OK", RT_TRUE,
                         "response", "Conversation reset successfully");
        goto cleanup;
    }

    item = cJSON_GetObjectItem(request_json, "message");
    if (item == RT_NULL || !cJSON_IsString(item))
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "message must be a string");
        goto cleanup;
    }

    user_message = cJSON_GetStringValue(item);
    if (user_message == RT_NULL || user_message[0] == '\0')
    {
        send_simple_json(session, 400, "Bad Request", RT_FALSE,
                         "error", "message cannot be empty");
        goto cleanup;
    }

    if (message_hub == RT_NULL ||
        message_hub->put_message == RT_NULL ||
        message_hub->get_message == RT_NULL)
    {
        send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                         "error", "Message hub is unavailable");
        goto cleanup;
    }

    messages_append(input_messages, TYPE_TEXT, user_message);

    if (input_messages == RT_NULL)
    {
        send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                         "error", "Failed to create input message");
        goto cleanup;
    }

    message_hub->put_message(message_hub, input_messages,
                             message_hub->input_mailbox);
    output_message = message_hub->get_message(message_hub,
                                              message_hub->output_mailbox);

    if (output_message == RT_NULL || messages_get_content_idx(output_message, 0) == RT_NULL)
    {
        send_simple_json(session, 502, "Bad Gateway", RT_FALSE,
                         "error", "LLM response failed");
        goto cleanup;
    }

    ai_reply = messages_get_content_idx(output_message, 0);

    if (stream_mode)
    {
        session->request->result_code = 200;
        webnet_session_set_header(session, "text/event-stream", 200, "OK", -1);
        webnet_session_write(session,
                             (const rt_uint8_t *)":rt-thread-stream\n\n",
                             strlen(":rt-thread-stream\n\n"));

        agent_webnet_stream_send_event(session, "delta", ai_reply);
        agent_webnet_stream_send_event(session, "final", ai_reply);
        agent_webnet_stream_send_event(session, "done", "[DONE]");
        goto cleanup;
    }

    send_simple_json(session, 200, "OK", RT_TRUE, "response", ai_reply);

cleanup:
    if (output_message != RT_NULL)
    {
        messages_destroy(output_message);
    }
    if (input_messages != RT_NULL)
    {
        messages_destroy(input_messages);
    }
    if (request_json != RT_NULL)
    {
        cJSON_Delete(request_json);
    }
    if (body != RT_NULL)
    {
        rt_free(body);
    }
}

/*
 * @brief 显示当前 LLM 配置（MSH 命令）
 */
static void show_llm_config(void)
{
    const char *api_key = get_dynamic_agent_api_key();

    LOG_I("=== Current LLM Configuration ===");
    LOG_I("API Key: %s", api_key != RT_NULL && api_key[0] != '\0'
                         ? "SET" : "NOT SET");
    LOG_I("Model Name: %s", safe_string(get_dynamic_agent_model_name()));
    LOG_I("API URL: %s", safe_string(get_dynamic_agent_api_url()));
    LOG_I("Message Hub: %s", message_hub != RT_NULL ? "READY" : "NULL");
    LOG_I("Context: %s", context != RT_NULL ? "READY" : "NULL");
    LOG_I("================================");
}
MSH_CMD_EXPORT(show_llm_config, Show current LLM configuration);

/*
 * @brief WebNet 通道初始化
 * @param hub 消息中心句柄
 * @param ctx 上下文管理器句柄
 */
void webnet_agent_mode(MessageHub_t hub, Context_t ctx)
{
    if (hub == RT_NULL || ctx == RT_NULL)
    {
        LOG_E("WebNet LLM mode requires a valid message hub and context");
        return;
    }

    message_hub = hub;
    context = ctx;

    if (webnet_started)
    {
        LOG_W("WebNet LLM mode is already started");
        return;
    }

    webnet_cgi_register("chat", cgi_chat_handler);
    webnet_cgi_register("config", cgi_config_handler);
    webnet_cgi_register("get_config", cgi_get_config_handler);

    webnet_init();
    webnet_started = RT_TRUE;
    show_llm_config();
}