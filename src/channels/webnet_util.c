#include "webnet_util.h"
#include <string.h>

const char *agent_wn_safe_string(const char *value)
{
    return value != RT_NULL ? value : "";
}

void agent_wn_send_raw_json(struct webnet_session *session, int status_code,
                            const char *reason, const char *body)
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

rt_err_t agent_wn_send_json_object(struct webnet_session *session, int status_code,
                                   const char *reason, cJSON *json)
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

    agent_wn_send_raw_json(session, status_code, reason, body);
    cJSON_free(body);
    return RT_EOK;
}

void agent_wn_send_simple_json(struct webnet_session *session, int status_code,
                               const char *reason, rt_bool_t success,
                               const char *field, const char *message)
{
    cJSON *json = cJSON_CreateObject();

    if (json == RT_NULL)
    {
        agent_wn_send_raw_json(session, 500, "Internal Server Error",
                               "{\"success\":false,\"error\":\"Failed to build response\"}");
        return;
    }

    /* cJSON_False 是类型位标志 (1<<0)，值为 1，是真值；
   这里直接传 rt_bool_t 的 0/1，否则永远序列化成 true */
    cJSON_AddBoolToObject(json, "success", success);
    if (field != RT_NULL && message != RT_NULL)
    {
        cJSON_AddStringToObject(json, field, message);
    }

    if (agent_wn_send_json_object(session, status_code, reason, json) != RT_EOK)
    {
        agent_wn_send_raw_json(session, 500, "Internal Server Error",
                               "{\"success\":false,\"error\":\"Failed to serialize response\"}");
    }

    cJSON_Delete(json);
}

char *agent_wn_copy_request_body(struct webnet_session *session, rt_size_t max_size)
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

rt_bool_t agent_wn_json_value_is_true(const cJSON *item)
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

void agent_wn_send_sse_event(struct webnet_session *session, const char *event,
                             const char *data)
{
    const char *cursor;

    if (session == RT_NULL)
    {
        return;
    }

    if (event != RT_NULL && event[0] != "\0")
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
        const char *newline = strchr(cursor, "\n");
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
