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
 * @brief 直接写出 OpenAI 请求体
 * @param writer     输出端（buf 为 NULL 时只累计长度）
 * @param src        对话内容来源
 * @param tools      工具定义数组
 * @param max_tokens 最大输出 token
 * @return RT_EOK 成功
 */
static rt_err_t openai_write_request(agent_json_writer_t *writer, const agent_chat_src_t *src,
                                     cJSON *tools, int max_tokens)
{
    if (!writer || !src)
    {
        return -RT_EINVAL;
    }

    agent_json_write_literal(writer, "{\"model\":");
    agent_json_write_string(writer, get_dynamic_agent_model_name());

    agent_json_write_literal(writer, ",\"messages\":");
    agent_context_write_messages(writer, src);

    if (tools != RT_NULL)
    {
        agent_json_write_literal(writer, ",\"tools\":");
        agent_json_write_cjson(writer, tools);
    }

    agent_json_write_literal(writer, ",\"max_tokens\":");
    agent_json_write_int(writer, max_tokens);
    agent_json_write_literal(writer, ",\"stream\":");
    agent_json_write_bool(writer, RT_TRUE);
    agent_json_write_literal(writer, "}");

    return agent_json_writer_ok(writer) ? RT_EOK : -RT_ENOMEM;
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

/* 关心的字段路径，顺序与 openai_watch_t 对应 */
enum
{
    OPENAI_W_ERROR = 0,
    OPENAI_W_CHOICES,
    OPENAI_W_CHOICE,
    OPENAI_W_FINISH,
    OPENAI_W_DELTA,
    OPENAI_W_REASONING_CONTENT,
    OPENAI_W_REASONING,
    OPENAI_W_CONTENT,
    OPENAI_W_TOOL_CALLS,
    OPENAI_W_TOOL_ITEM,
    OPENAI_W_TOOL_INDEX,
    OPENAI_W_TOOL_ID,
    OPENAI_W_TOOL_FUNCTION,
    OPENAI_W_TOOL_NAME,
    OPENAI_W_TOOL_ARGS
};

static const char *const g_openai_watches[] =
{
    "error",
    "choices",
    "choices.0",
    "choices.0.finish_reason",
    "choices.0.delta",
    "choices.0.delta.reasoning_content",
    "choices.0.delta.reasoning",
    "choices.0.delta.content",
    "choices.0.delta.tool_calls",
    "choices.0.delta.tool_calls.*",
    "choices.0.delta.tool_calls.*.index",
    "choices.0.delta.tool_calls.*.id",
    "choices.0.delta.tool_calls.*.function",
    "choices.0.delta.tool_calls.*.function.name",
    "choices.0.delta.tool_calls.*.function.arguments"
};

#define OPENAI_MAX_REASON 16

typedef struct
{
    agent_protocol_stream_t *stream;
    rt_bool_t saw_choices;
    rt_bool_t choice_seen;
    rt_bool_t delta_object;
    rt_bool_t finish_string;
    char finish[OPENAI_MAX_REASON];
    int finish_len;
    int tool_index;
    char tool_id[AGENT_PROTOCOL_TOOL_ID_LEN];
    int tool_id_len;
    char tool_name[AGENT_PROTOCOL_TOOL_NAME_LEN];
    int tool_name_len;
} openai_event_t;

static void openai_finish_reason(openai_event_t *ev)
{
    agent_protocol_stream_t *stream = ev->stream;

    ev->finish_string = RT_TRUE;
    ev->finish[ev->finish_len] = '\0';

    if (rt_strcmp(ev->finish, "stop") == 0)
    {
        stream->natural_stop = RT_TRUE;
        return;
    }

    /* length：写到 max_tokens 就停了，内容本身是好的，只是没写完 */
    if (rt_strcmp(ev->finish, "length") == 0)
    {
        stream->truncated = RT_TRUE;
    }
    stream->content_lost = RT_TRUE;
}

static void openai_on_match(void *user, int watch, const agent_json_match_t *match)
{
    openai_event_t *ev = (openai_event_t *)user;
    agent_protocol_stream_t *stream = ev->stream;

    switch (watch)
    {
    case OPENAI_W_ERROR:
        stream->content_lost = RT_TRUE;
        break;

    case OPENAI_W_CHOICES:
        ev->saw_choices = RT_TRUE;
        if (match->type != AGENT_JSON_VALUE_ARRAY)
        {
            stream->content_lost = RT_TRUE;
        }
        break;

    case OPENAI_W_CHOICE:
        ev->choice_seen = RT_TRUE;
        if (match->type != AGENT_JSON_VALUE_OBJECT)
        {
            stream->content_lost = RT_TRUE;
        }
        break;

    case OPENAI_W_FINISH:
        if (match->type != AGENT_JSON_VALUE_STRING)
        {
            break;
        }
        agent_protocol_text_append(ev->finish, sizeof(ev->finish), &ev->finish_len,
                                   match->text, match->len);
        if (match->last)
        {
            openai_finish_reason(ev);
        }
        break;

    case OPENAI_W_DELTA:
        if (match->type == AGENT_JSON_VALUE_OBJECT)
        {
            ev->delta_object = RT_TRUE;
        }
        break;

    case OPENAI_W_REASONING_CONTENT:
    case OPENAI_W_REASONING:
        if (match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_reasoning(stream, match->text);
        }
        break;

    case OPENAI_W_CONTENT:
        if (match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_content(stream, match->text);
        }
        else if (match->type != AGENT_JSON_VALUE_NULL)
        {
            stream->content_lost = RT_TRUE;
        }
        break;

    case OPENAI_W_TOOL_CALLS:
        /* null 视为没有工具调用；不是数组也同样按出现工具调用处理 */
        if (match->type != AGENT_JSON_VALUE_ARRAY && match->type != AGENT_JSON_VALUE_NULL)
        {
            stream->saw_tool_call = RT_TRUE;
        }
        break;

    case OPENAI_W_TOOL_ITEM:
        stream->saw_tool_call = RT_TRUE;
        ev->tool_index = (match->index_count > 0) ? match->index[0] : 0;
        ev->tool_id_len = 0;
        ev->tool_name_len = 0;
        break;

    case OPENAI_W_TOOL_INDEX:
        if (match->type == AGENT_JSON_VALUE_NUMBER)
        {
            ev->tool_index = match->number;
        }
        break;

    case OPENAI_W_TOOL_ID:
        if (match->type != AGENT_JSON_VALUE_STRING)
        {
            break;
        }
        if (agent_protocol_text_append(ev->tool_id, sizeof(ev->tool_id), &ev->tool_id_len,
                                       match->text, match->len))
        {
            stream->content_lost = RT_TRUE;
        }
        if (match->last)
        {
            agent_protocol_tool_begin(stream, ev->tool_index, ev->tool_id, RT_NULL);
            ev->tool_id_len = 0;
        }
        break;

    case OPENAI_W_TOOL_FUNCTION:
        break;

    case OPENAI_W_TOOL_NAME:
        if (match->type != AGENT_JSON_VALUE_STRING)
        {
            break;
        }
        if (agent_protocol_text_append(ev->tool_name, sizeof(ev->tool_name), &ev->tool_name_len,
                                       match->text, match->len))
        {
            stream->content_lost = RT_TRUE;
        }
        if (match->last)
        {
            agent_protocol_tool_begin(stream, ev->tool_index, RT_NULL, ev->tool_name);
            ev->tool_name_len = 0;
        }
        break;

    case OPENAI_W_TOOL_ARGS:
        if (match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_tool_args(stream, ev->tool_index, match->text);
        }
        break;

    default:
        break;
    }
}

/*
 * @brief 解析一条 OpenAI SSE 事件
 * @param json   事件 JSON 文本
 * @param len    文本长度
 * @param stream 流状态
 * @return 恒为 RT_FALSE，OpenAI 用 data: [DONE] 收尾
 */
static rt_bool_t openai_parse_event(const char *json, int len, agent_protocol_stream_t *stream)
{
    openai_event_t ev;

    if (!json || !stream)
    {
        return RT_FALSE;
    }

    rt_memset(&ev, 0, sizeof(ev));
    ev.stream = stream;

    if (agent_json_scan(json, len, g_openai_watches,
                        sizeof(g_openai_watches) / sizeof(g_openai_watches[0]),
                        openai_on_match, &ev) != RT_EOK)
    {
        stream->content_lost = RT_TRUE;
        return RT_FALSE;
    }

    /* 没有 choices 的事件，或 choice 里既没有 delta 也没有 finish_reason，都算异常报文 */
    if (!ev.saw_choices)
    {
        stream->content_lost = RT_TRUE;
    }
    if (ev.choice_seen && !ev.delta_object && !ev.finish_string)
    {
        stream->content_lost = RT_TRUE;
    }

    return RT_FALSE;
}

static const agent_protocol_ops_t g_openai_ops =
{
    .name = "openai",
    .write_request = openai_write_request,
    .add_headers = openai_add_headers,
    .parse_event = openai_parse_event,
};

/*
 * @brief 取 OpenAI 协议的操作表
 * @return 操作表指针
 */
const agent_protocol_ops_t *agent_protocol_openai_ops(void)
{
    return &g_openai_ops;
}
