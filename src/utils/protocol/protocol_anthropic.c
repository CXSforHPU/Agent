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
 * @brief 在消息记录里找第一个指定类型的块
 * @param item 消息项
 * @param type 块类型
 * @return 块指针，没有返回 NULL
 */
static const agent_ctx_block_t *__find_block(const agent_ctx_item_t *item, int type)
{
    const agent_ctx_block_t *blocks;
    int i;

    if (!item->view) return RT_NULL;
    blocks = agent_ctx_view_blocks(item->view);
    for (i = 0; i < item->view->block_count; i++)
    {
        if (blocks[i].type == type)
        {
            return &blocks[i];
        }
    }
    return RT_NULL;
}

/*
 * @brief 取 TEXT 块的正文
 * @param item 消息项
 * @return 正文，没有 TEXT 块返回空串
 */
static const char *__view_text(const agent_ctx_item_t *item)
{
    const agent_ctx_block_t *block;

    if (!item->view) return item->text ? item->text : "";
    block = __find_block(item, AGENT_CTX_BLOCK_TEXT);

    return block ? agent_ctx_view_slice(item->view, &block->first) : "";
}

/*
 * @brief 写 user 消息的 content 块数组内容
 * @param writer JSON 输出端
 * @param item   消息项
 */
static void __write_user_blocks(agent_json_writer_t *writer, const agent_ctx_item_t *item)
{
    const agent_ctx_block_t *blocks;
    rt_bool_t first = RT_TRUE;
    int i;

    /* 纯文本消息（system_override/user_text）没有记录视图，直接写一个 text 块 */
    if (!item->view)
    {
        agent_json_write_literal(writer, "{\"type\":\"text\",\"text\":");
        agent_json_write_string(writer, item->text ? item->text : "");
        agent_json_write_literal(writer, "}");
        return;
    }

    blocks = agent_ctx_view_blocks(item->view);
    for (i = 0; i < item->view->block_count; i++)
    {
        const agent_ctx_block_t *block = &blocks[i];
        const char *url = agent_ctx_view_slice(item->view, &block->first);

        /* 空 URL 的图片发出去没有意义，也避免写出多余的逗号 */
        if (block->type == AGENT_CTX_BLOCK_IMAGE && (!url || !url[0])) continue;
        if (block->type != AGENT_CTX_BLOCK_TEXT &&
            block->type != AGENT_CTX_BLOCK_IMAGE &&
            block->type != AGENT_CTX_BLOCK_AUDIO &&
            block->type != AGENT_CTX_BLOCK_VIDEO) continue;

        if (!first) agent_json_write_literal(writer, ",");
        first = RT_FALSE;

        if (block->type == AGENT_CTX_BLOCK_TEXT)
        {
            agent_json_write_literal(writer, "{\"type\":\"text\",\"text\":");
            agent_json_write_string(writer, url);
            agent_json_write_literal(writer, "}");
        }
        else if (block->type == AGENT_CTX_BLOCK_IMAGE)
        {
            agent_json_write_literal(writer, "{\"type\":\"image\",\"source\":{\"type\":\"url\",\"url\":");
            agent_json_write_string(writer, url);
            agent_json_write_literal(writer, "}}");
        }
        else
        {
            /* Anthropic 的 Messages 接口不收内联音视频，换成一句说明 */
            LOG_W("anthropic: %s block not supported, replaced by a text note",
                  block->type == AGENT_CTX_BLOCK_AUDIO ? "audio_url" : "video_url");
            agent_json_write_literal(writer, "{\"type\":\"text\",\"text\":");
            agent_json_write_string(writer, "[音视频内容在 Anthropic 协议下未发送]");
            agent_json_write_literal(writer, "}");
        }
    }
}

/*
 * @brief 写 tool_use 块的 input 字段
 * @param writer    JSON 输出端
 * @param arguments 工具参数 JSON 文本
 */
static void __write_tool_input(agent_json_writer_t *writer, const char *arguments)
{
    if (arguments && arguments[0])
    {
        cJSON *input = cJSON_Parse(arguments);

        if (input && cJSON_IsObject(input))
        {
            agent_json_write_cjson(writer, input);
            cJSON_Delete(input);
            return;
        }
        cJSON_Delete(input);
    }
    agent_json_write_literal(writer, "{}");
}

/*
 * @brief 判断 assistant 消息有没有可发送的内容
 *
 * 空正文又没有工具调用的 assistant 消息发出去会被接口拒绝，直接跳过。
 * @param item 消息项
 * @return RT_TRUE 有内容
 */
static rt_bool_t __assistant_has_output(const agent_ctx_item_t *item)
{
    const agent_ctx_block_t *blocks = item->view ? agent_ctx_view_blocks(item->view) : RT_NULL;
    int i;

    if (__view_text(item)[0]) return RT_TRUE;
    if (!blocks) return RT_FALSE;
    for (i = 0; i < item->view->block_count; i++)
    {
        if (blocks[i].type == AGENT_CTX_BLOCK_TOOL_CALL) return RT_TRUE;
    }
    return RT_FALSE;
}

/*
 * @brief 写 assistant 消息的 content 块数组内容
 * @param writer JSON 输出端
 * @param item   消息项
 */
static void __write_assistant_blocks(agent_json_writer_t *writer, const agent_ctx_item_t *item)
{
    const char *text = __view_text(item);
    const agent_ctx_block_t *blocks;
    rt_bool_t first = RT_TRUE;
    int i;

    if (text[0])
    {
        agent_json_write_literal(writer, "{\"type\":\"text\",\"text\":");
        agent_json_write_string(writer, text);
        agent_json_write_literal(writer, "}");
        first = RT_FALSE;
    }

    if (!item->view) return;

    blocks = agent_ctx_view_blocks(item->view);
    for (i = 0; i < item->view->block_count; i++)
    {
        const agent_ctx_block_t *block = &blocks[i];

        if (block->type != AGENT_CTX_BLOCK_TOOL_CALL) continue;
        if (!first) agent_json_write_literal(writer, ",");
        first = RT_FALSE;

        agent_json_write_literal(writer, "{\"type\":\"tool_use\",\"id\":");
        agent_json_write_string(writer, agent_ctx_view_slice(item->view, &block->first));
        agent_json_write_literal(writer, ",\"name\":");
        agent_json_write_string(writer, agent_ctx_view_slice(item->view, &block->second));
        agent_json_write_literal(writer, ",\"input\":");
        __write_tool_input(writer, agent_ctx_view_slice(item->view, &block->third));
        agent_json_write_literal(writer, "}");
    }
}

/*
 * @brief 写一个 tool_result 块
 * @param writer JSON 输出端
 * @param item   消息项
 */
static void __write_tool_result(agent_json_writer_t *writer, const agent_ctx_item_t *item)
{
    const agent_ctx_block_t *block = __find_block(item, AGENT_CTX_BLOCK_TOOL_RESULT);

    agent_json_write_literal(writer, "{\"type\":\"tool_result\",\"tool_use_id\":");
    agent_json_write_string(writer, block ? agent_ctx_view_slice(item->view, &block->first) : "");
    agent_json_write_literal(writer, ",\"content\":");
    agent_json_write_string(writer, __view_text(item));
    agent_json_write_literal(writer, "}");
}

/*
 * @brief 把 OpenAI 风格的工具定义直接写成 Anthropic 的格式
 * @param writer JSON 输出端
 * @param tools  OpenAI 风格工具数组
 */
static void __write_tools(agent_json_writer_t *writer, cJSON *tools)
{
    int size = cJSON_GetArraySize(tools);
    rt_bool_t first = RT_TRUE;
    int i;

    agent_json_write_literal(writer, ",\"tools\":[");
    for (i = 0; i < size; i++)
    {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *func;
        cJSON *params;
        const char *name;
        const char *description;

        if (!cJSON_IsObject(tool)) continue;

        func = cJSON_GetObjectItemCaseSensitive(tool, "function");
        params = cJSON_GetObjectItemCaseSensitive(func, "parameters");
        name = get_string(func, "name");
        description = get_string(func, "description");

        if (!first) agent_json_write_literal(writer, ",");
        first = RT_FALSE;

        agent_json_write_literal(writer, "{\"name\":");
        agent_json_write_string(writer, name ? name : "");
        if (description)
        {
            agent_json_write_literal(writer, ",\"description\":");
            agent_json_write_string(writer, description);
        }
        agent_json_write_literal(writer, ",\"input_schema\":");
        if (params && cJSON_IsObject(params))
        {
            agent_json_write_cjson(writer, params);
        }
        else
        {
            agent_json_write_literal(writer, "{\"type\":\"object\",\"properties\":{}}");
        }
        agent_json_write_literal(writer, "}");
    }
    agent_json_write_literal(writer, "]");
}

/*
 * @brief 直接写出 Anthropic 请求体
 * @param writer     输出端（buf 为 NULL 时只累计长度）
 * @param src        对话内容来源
 * @param tools      OpenAI 风格工具数组
 * @param max_tokens 最大输出 token
 * @return RT_EOK 成功
 */
static rt_err_t anthropic_write_request(agent_json_writer_t *writer, const agent_chat_src_t *src,
                                        cJSON *tools, int max_tokens)
{
    agent_ctx_cursor_t cursor;
    agent_ctx_item_t item;
    const char *system_text;
    rt_bool_t first_msg = RT_TRUE;
    rt_bool_t open_tool_result = RT_FALSE;
    int limit = max_tokens;

    if (!writer || !src)
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

    system_text = src->system_override;
    if (!system_text)
    {
        system_text = agent_context_system_prompt(src->ctx);
    }

    agent_json_write_literal(writer, "{\"model\":");
    agent_json_write_string(writer, get_dynamic_agent_model_name());
    agent_json_write_literal(writer, ",\"max_tokens\":");
    agent_json_write_int(writer, limit);
    agent_json_write_literal(writer, ",\"stream\":");
    agent_json_write_bool(writer, RT_TRUE);

    if (system_text && system_text[0])
    {
        agent_json_write_literal(writer, ",\"system\":");
        agent_json_write_string(writer, system_text);
    }

    agent_json_write_literal(writer, ",\"messages\":[");
    agent_ctx_cursor_init(&cursor, src);

    while (agent_ctx_cursor_next(&cursor, &item))
    {
        /* system 已写到顶层字段，消息数组里不再出现 */
        if (item.role == ROLE_SYSTEM) continue;

        if (item.role == ROLE_TOOL)
        {
            /*
             * 连续的 tool 结果要合并进同一条 user 消息，
             * Anthropic 不接受连续同角色消息。
             */
            if (open_tool_result)
            {
                agent_json_write_literal(writer, ",");
            }
            else
            {
                if (!first_msg) agent_json_write_literal(writer, ",");
                first_msg = RT_FALSE;
                agent_json_write_literal(writer, "{\"role\":\"user\",\"content\":[");
                open_tool_result = RT_TRUE;
            }
            __write_tool_result(writer, &item);
            continue;
        }

        if (open_tool_result)
        {
            agent_json_write_literal(writer, "]}");
            open_tool_result = RT_FALSE;
        }

        if (item.role == ROLE_USER)
        {
            if (!first_msg) agent_json_write_literal(writer, ",");
            first_msg = RT_FALSE;
            agent_json_write_literal(writer, "{\"role\":\"user\",\"content\":[");
            __write_user_blocks(writer, &item);
            agent_json_write_literal(writer, "]}");
        }
        else
        {
            if (!__assistant_has_output(&item)) continue;

            if (!first_msg) agent_json_write_literal(writer, ",");
            first_msg = RT_FALSE;
            agent_json_write_literal(writer, "{\"role\":\"assistant\",\"content\":[");
            __write_assistant_blocks(writer, &item);
            agent_json_write_literal(writer, "]}");
        }
    }

    if (open_tool_result)
    {
        agent_json_write_literal(writer, "]}");
    }
    agent_json_write_literal(writer, "]");

    if (tools != RT_NULL)
    {
        __write_tools(writer, tools);
    }

    agent_json_write_literal(writer, "}");

    return agent_json_writer_ok(writer) ? RT_EOK : -RT_ENOMEM;
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

/* 关心的字段路径，顺序与 anthropic_watch_t 对应 */
enum
{
    ANTHROPIC_W_TYPE = 0,
    ANTHROPIC_W_INDEX,
    ANTHROPIC_W_BLOCK,
    ANTHROPIC_W_BLOCK_TYPE,
    ANTHROPIC_W_BLOCK_TEXT,
    ANTHROPIC_W_BLOCK_THINKING,
    ANTHROPIC_W_BLOCK_ID,
    ANTHROPIC_W_BLOCK_NAME,
    ANTHROPIC_W_DELTA,
    ANTHROPIC_W_DELTA_TYPE,
    ANTHROPIC_W_DELTA_TEXT,
    ANTHROPIC_W_DELTA_THINKING,
    ANTHROPIC_W_DELTA_JSON,
    ANTHROPIC_W_STOP_REASON
};

static const char *const g_anthropic_watches[] =
{
    "type",
    "index",
    "content_block",
    "content_block.type",
    "content_block.text",
    "content_block.thinking",
    "content_block.id",
    "content_block.name",
    "delta",
    "delta.type",
    "delta.text",
    "delta.thinking",
    "delta.partial_json",
    "delta.stop_reason"
};

#define ANTHROPIC_TAG_LEN 24

typedef enum
{
    ANTHROPIC_EVENT_OTHER = 0,
    ANTHROPIC_EVENT_BLOCK_START,
    ANTHROPIC_EVENT_BLOCK_DELTA,
    ANTHROPIC_EVENT_BLOCK_STOP,
    ANTHROPIC_EVENT_MESSAGE_DELTA,
    ANTHROPIC_EVENT_MESSAGE_STOP,
    ANTHROPIC_EVENT_ERROR
} anthropic_event_kind_t;

typedef enum
{
    ANTHROPIC_BLOCK_OTHER = 0,
    ANTHROPIC_BLOCK_TEXT,
    ANTHROPIC_BLOCK_TOOL_USE,
    ANTHROPIC_BLOCK_THINKING
} anthropic_block_kind_t;

typedef enum
{
    ANTHROPIC_DELTA_OTHER = 0,
    ANTHROPIC_DELTA_TEXT,
    ANTHROPIC_DELTA_THINKING,
    ANTHROPIC_DELTA_JSON
} anthropic_delta_kind_t;

typedef struct
{
    agent_protocol_stream_t *stream;
    rt_bool_t saw_type;
    anthropic_event_kind_t event;
    anthropic_block_kind_t block;
    anthropic_delta_kind_t delta;
    int index;
    rt_bool_t done;
    char tag[ANTHROPIC_TAG_LEN];
    int tag_len;
    char stop[ANTHROPIC_TAG_LEN];
    int stop_len;
    char tool_id[AGENT_PROTOCOL_TOOL_ID_LEN];
    int tool_id_len;
    char tool_name[AGENT_PROTOCOL_TOOL_NAME_LEN];
    int tool_name_len;
} anthropic_event_t;

static anthropic_event_kind_t anthropic_event_of(const char *text)
{
    if (rt_strcmp(text, "content_block_start") == 0) return ANTHROPIC_EVENT_BLOCK_START;
    if (rt_strcmp(text, "content_block_delta") == 0) return ANTHROPIC_EVENT_BLOCK_DELTA;
    if (rt_strcmp(text, "content_block_stop") == 0) return ANTHROPIC_EVENT_BLOCK_STOP;
    if (rt_strcmp(text, "message_delta") == 0) return ANTHROPIC_EVENT_MESSAGE_DELTA;
    if (rt_strcmp(text, "message_stop") == 0) return ANTHROPIC_EVENT_MESSAGE_STOP;
    if (rt_strcmp(text, "error") == 0) return ANTHROPIC_EVENT_ERROR;

    return ANTHROPIC_EVENT_OTHER;
}

static anthropic_block_kind_t anthropic_block_of(const char *text)
{
    if (rt_strcmp(text, "text") == 0) return ANTHROPIC_BLOCK_TEXT;
    if (rt_strcmp(text, "tool_use") == 0) return ANTHROPIC_BLOCK_TOOL_USE;
    if (rt_strcmp(text, "thinking") == 0) return ANTHROPIC_BLOCK_THINKING;

    return ANTHROPIC_BLOCK_OTHER;
}

static anthropic_delta_kind_t anthropic_delta_of(const char *text)
{
    if (rt_strcmp(text, "text_delta") == 0) return ANTHROPIC_DELTA_TEXT;
    if (rt_strcmp(text, "thinking_delta") == 0) return ANTHROPIC_DELTA_THINKING;
    if (rt_strcmp(text, "input_json_delta") == 0) return ANTHROPIC_DELTA_JSON;

    return ANTHROPIC_DELTA_OTHER;
}

static void anthropic_stop_reason(anthropic_event_t *ev)
{
    agent_protocol_stream_t *stream = ev->stream;

    ev->stop[ev->stop_len] = '\0';

    if (rt_strcmp(ev->stop, "end_turn") == 0)
    {
        stream->natural_stop = RT_TRUE;
    }
    else if (rt_strcmp(ev->stop, "tool_use") == 0)
    {
        stream->saw_tool_call = RT_TRUE;
    }
    else if (rt_strcmp(ev->stop, "max_tokens") == 0)
    {
        /* 写到 max_tokens 就停了，内容本身是好的，只是没写完 */
        stream->truncated = RT_TRUE;
        stream->content_lost = RT_TRUE;
    }
    else
    {
        /* stop_sequence / refusal 等都不算完整收尾 */
        stream->content_lost = RT_TRUE;
    }
    ev->stop_len = 0;
}

/*
 * @brief 累积事件/块/增量类型标签，结束时返回完整值
 * @return RT_TRUE 字符串已完整
 */
static rt_bool_t anthropic_tag_done(anthropic_event_t *ev, const agent_json_match_t *match)
{
    if (agent_protocol_text_append(ev->tag, sizeof(ev->tag), &ev->tag_len,
                                   match->text, match->len))
    {
        ev->stream->content_lost = RT_TRUE;
    }
    if (!match->last)
    {
        return RT_FALSE;
    }
    ev->tag[ev->tag_len] = '\0';

    return RT_TRUE;
}

static void anthropic_on_match(void *user, int watch, const agent_json_match_t *match)
{
    anthropic_event_t *ev = (anthropic_event_t *)user;
    agent_protocol_stream_t *stream = ev->stream;

    switch (watch)
    {
    case ANTHROPIC_W_TYPE:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        ev->saw_type = RT_TRUE;
        if (anthropic_tag_done(ev, match))
        {
            ev->event = anthropic_event_of(ev->tag);
            ev->tag_len = 0;

            if (ev->event == ANTHROPIC_EVENT_MESSAGE_STOP)
            {
                stream->stream_done = RT_TRUE;
                ev->done = RT_TRUE;
            }
            else if (ev->event == ANTHROPIC_EVENT_ERROR)
            {
                LOG_E("anthropic: error event received");
                stream->content_lost = RT_TRUE;
            }
        }
        break;

    case ANTHROPIC_W_INDEX:
        if (match->type == AGENT_JSON_VALUE_NUMBER)
        {
            ev->index = match->number;
        }
        break;

    case ANTHROPIC_W_BLOCK:
        ev->block = ANTHROPIC_BLOCK_OTHER;
        ev->tool_id_len = 0;
        ev->tool_name_len = 0;
        break;

    case ANTHROPIC_W_BLOCK_TYPE:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        if (anthropic_tag_done(ev, match))
        {
            ev->block = anthropic_block_of(ev->tag);
            ev->tag_len = 0;

            if (ev->block == ANTHROPIC_BLOCK_TOOL_USE)
            {
                stream->saw_tool_call = RT_TRUE;
            }
        }
        break;

    case ANTHROPIC_W_BLOCK_TEXT:
        if (ev->block == ANTHROPIC_BLOCK_TEXT && match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_content(stream, match->text);
        }
        break;

    case ANTHROPIC_W_BLOCK_THINKING:
        if (ev->block == ANTHROPIC_BLOCK_THINKING && match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_reasoning(stream, match->text);
        }
        break;

    case ANTHROPIC_W_BLOCK_ID:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        if (agent_protocol_text_append(ev->tool_id, sizeof(ev->tool_id), &ev->tool_id_len,
                                       match->text, match->len))
        {
            stream->content_lost = RT_TRUE;
        }
        if (match->last)
        {
            agent_protocol_tool_begin(stream, ev->index, ev->tool_id, RT_NULL);
            ev->tool_id_len = 0;
        }
        break;

    case ANTHROPIC_W_BLOCK_NAME:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        if (agent_protocol_text_append(ev->tool_name, sizeof(ev->tool_name), &ev->tool_name_len,
                                       match->text, match->len))
        {
            stream->content_lost = RT_TRUE;
        }
        if (match->last)
        {
            agent_protocol_tool_begin(stream, ev->index, RT_NULL, ev->tool_name);
            ev->tool_name_len = 0;
        }
        break;

    case ANTHROPIC_W_DELTA:
        ev->delta = ANTHROPIC_DELTA_OTHER;
        break;

    case ANTHROPIC_W_DELTA_TYPE:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        if (anthropic_tag_done(ev, match))
        {
            ev->delta = anthropic_delta_of(ev->tag);
            ev->tag_len = 0;
        }
        break;

    case ANTHROPIC_W_DELTA_TEXT:
        if (ev->delta == ANTHROPIC_DELTA_TEXT && match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_content(stream, match->text);
        }
        break;

    case ANTHROPIC_W_DELTA_THINKING:
        if (ev->delta == ANTHROPIC_DELTA_THINKING && match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_add_reasoning(stream, match->text);
        }
        break;

    case ANTHROPIC_W_DELTA_JSON:
        if (ev->delta == ANTHROPIC_DELTA_JSON && match->type == AGENT_JSON_VALUE_STRING)
        {
            agent_protocol_tool_args(stream, ev->index, match->text);
        }
        break;

    case ANTHROPIC_W_STOP_REASON:
        if (match->type != AGENT_JSON_VALUE_STRING) break;
        agent_protocol_text_append(ev->stop, sizeof(ev->stop), &ev->stop_len,
                                   match->text, match->len);
        if (match->last)
        {
            anthropic_stop_reason(ev);
        }
        break;

    default:
        break;
    }
}

/*
 * @brief 解析一条 Anthropic SSE 事件
 * @param json   事件 JSON 文本
 * @param len    文本长度
 * @param stream 流状态
 * @return RT_TRUE 表示流已结束
 */
static rt_bool_t anthropic_parse_event(const char *json, int len, agent_protocol_stream_t *stream)
{
    anthropic_event_t ev;

    if (!json || !stream)
    {
        return RT_FALSE;
    }

    rt_memset(&ev, 0, sizeof(ev));
    ev.stream = stream;

    if (agent_json_scan(json, len, g_anthropic_watches,
                        sizeof(g_anthropic_watches) / sizeof(g_anthropic_watches[0]),
                        anthropic_on_match, &ev) != RT_EOK)
    {
        stream->content_lost = RT_TRUE;
        return RT_FALSE;
    }

    if (!ev.saw_type)
    {
        stream->content_lost = RT_TRUE;
    }

    return ev.done;
}

static const agent_protocol_ops_t g_anthropic_ops =
{
    .name = "anthropic",
    .write_request = anthropic_write_request,
    .add_headers = anthropic_add_headers,
    .parse_event = anthropic_parse_event,
};

/*
 * @brief 取 Anthropic 协议的操作表
 * @return 操作表指针
 */
const agent_protocol_ops_t *agent_protocol_anthropic_ops(void)
{
    return &g_anthropic_ops;
}
