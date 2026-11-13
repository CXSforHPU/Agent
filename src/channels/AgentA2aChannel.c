#include "AgentA2aChannel.h"
#include "webnet_util.h"
#include "AgentStream.h"
#include "dfs_posix.h"
#include "unistd.h"

#define LOG_TAG "Agent.channel.a2a"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define A2A_REQUEST_MAX_SIZE 4096
#define A2A_REPLY_TIMEOUT_MS 30000
/* 等结果时的轮询间隔：既能及时把中间事件推给客户端，又不至于空转太频繁 */
#define A2A_POLL_INTERVAL_MS 100

/* 能力卡的运行时配置，控制台下发后落盘；路径由 Kconfig 决定，
   Kconfig 里那两个名字与描述只当第一次启动的默认值 */
#ifndef PKG_AGENT_A2A_CARD_PATH
#define PKG_AGENT_A2A_CARD_PATH    "/agent_card.json"
#endif
#define A2A_CARD_BODY_MAX   1024
#define A2A_CARD_NAME_MAX   64
#define A2A_CARD_DESC_MAX   256
#define A2A_CARD_VER_MAX    32

typedef struct
{
    char name[A2A_CARD_NAME_MAX];
    char description[A2A_CARD_DESC_MAX];
    char version[A2A_CARD_VER_MAX];
} a2a_card_config_t;

static a2a_card_config_t g_card;

/*
 * @brief 能力卡回落到编译期默认值
 */
static void card_set_defaults(void)
{
    rt_snprintf(g_card.name, sizeof(g_card.name), "%s", PKG_AGENT_A2A_NAME);
    rt_snprintf(g_card.description, sizeof(g_card.description), "%s",
                PKG_AGENT_A2A_DESCRIPTION);
    rt_snprintf(g_card.version, sizeof(g_card.version), "%s", "1.0.0");
}

/*
 * @brief 启动时读上次下发的能力卡，读不到就用默认值
 * @note  文件缺失是正常情况（第一次启动），不当错误报
 */
static void card_load(void)
{
    char buf[A2A_CARD_BODY_MAX];
    cJSON *root;
    cJSON *item;
    int fd;
    int len;

    card_set_defaults();

    fd = open(PKG_AGENT_A2A_CARD_PATH, O_RDONLY);
    if (fd < 0)
    {
        return;
    }
    len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
    {
        LOG_W("agent card file %s is empty, using defaults", PKG_AGENT_A2A_CARD_PATH);
        return;
    }
    buf[len] = '\0';

    root = cJSON_Parse(buf);
    if (root == RT_NULL)
    {
        LOG_W("agent card file %s is not valid JSON, using defaults", PKG_AGENT_A2A_CARD_PATH);
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item) && item->valuestring[0] != '\0')
    {
        rt_snprintf(g_card.name, sizeof(g_card.name), "%s", item->valuestring);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(item))
    {
        rt_snprintf(g_card.description, sizeof(g_card.description), "%s", item->valuestring);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(item) && item->valuestring[0] != '\0')
    {
        rt_snprintf(g_card.version, sizeof(g_card.version), "%s", item->valuestring);
    }

    cJSON_Delete(root);
    LOG_I("agent card loaded: name=%s, file=%s", g_card.name, PKG_AGENT_A2A_CARD_PATH);
}

/*
 * @brief 把当前能力卡写回 SD 卡，下次启动还是这份
 * @return RT_EOK 成功
 */
static rt_err_t card_save(void)
{
    cJSON *root;
    char *encoded;
    rt_size_t len;
    int fd;
    int written;

    root = cJSON_CreateObject();
    if (root == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(root, "name", g_card.name);
    cJSON_AddStringToObject(root, "description", g_card.description);
    cJSON_AddStringToObject(root, "version", g_card.version);

    encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    len = rt_strlen(encoded);

    fd = open(PKG_AGENT_A2A_CARD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        cJSON_free(encoded);
        return -RT_ERROR;
    }
    written = write(fd, encoded, len);
    close(fd);
    cJSON_free(encoded);

    return (written == (int)len) ? RT_EOK : -RT_ERROR;
}

static MessageHub_t message_hub = RT_NULL;
static Context_t context = RT_NULL;
static rt_bool_t started = RT_FALSE;
static rt_uint32_t task_seq = 0;

/*
 * @brief A2A 通道 ops.init 包装（统一返回 int）
 */
static int a2a_channel_init_wrapper(MessageHub_t hub, Context_t ctx)
{
    a2a_agent_channel_init(hub, ctx);
    return RT_EOK;
}

/*
 * @brief A2A 通道清理（置空缓存的 hub/context 指针，幂等）
 */
void agent_a2a_reset(void)
{
    message_hub = RT_NULL;
    context = RT_NULL;
}

AgentChannelOps agent_a2a_ops = { a2a_channel_init_wrapper, agent_a2a_reset };

/*
 * @brief 造一个任务 id，形如 task-1、task-2
 * @return 任务 id 字符串
 */
static const char *next_task_id(void)
{
    static char id[24];

    task_seq++;
    rt_snprintf(id, sizeof(id), "task-%u", (unsigned int)task_seq);
    return id;
}

/*
 * @brief 从请求体里取出用户消息文本
 * @param  session webnet 会话
 * @param  body    已复制的请求体
 * @return 消息文本，失败返回 NULL
 */
static char *extract_text(const char *body)
{
    cJSON *root;
    cJSON *message;
    cJSON *parts;
    cJSON *part;
    cJSON *text;
    char *copy;

    root = cJSON_Parse(body);
    if (root == RT_NULL)
    {
        return RT_NULL;
    }

    message = cJSON_GetObjectItemCaseSensitive(root, "message");
    parts = message != RT_NULL ? cJSON_GetObjectItemCaseSensitive(message, "parts") : RT_NULL;
    part = (parts != RT_NULL && cJSON_GetArraySize(parts) > 0)
         ? cJSON_GetArrayItem(parts, 0) : RT_NULL;
    text = part != RT_NULL ? cJSON_GetObjectItemCaseSensitive(part, "text") : RT_NULL;

    copy = (text != RT_NULL && cJSON_IsString(text) && text->valuestring != RT_NULL
            && text->valuestring[0] != '\0')
         ? rt_strdup(text->valuestring) : RT_NULL;

    cJSON_Delete(root);
    return copy;
}

/*
 * @brief 把用户消息投给 agent
 * @param text 用户输入
 * @return RT_EOK 成功
 */
static rt_err_t task_submit(const char *text)
{
    Messages_t input;

    if (message_hub == RT_NULL || message_hub->put_message == RT_NULL ||
        message_hub->get_message == RT_NULL)
    {
        return -RT_ERROR;
    }

    input = messages_create(1);
    if (input == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    if (messages_append(input, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(input);
        return -RT_ERROR;
    }

    /* put 成功后所有权移交 main_loop，本地置空防止误释放 */
    if (message_hub->put_message(message_hub, input, message_hub->input_mailbox) != RT_EOK)
    {
        messages_destroy(input);
        return -RT_ERROR;
    }

    return RT_EOK;
}

/*
 * @brief 造一条状态更新帧（思考、工具调用、正文分片）
 * @param id   任务 id
 * @param kind 事件类型名，reasoning / tool / content
 * @param text 文本内容
 * @return 任务结果，调用方负责 cJSON_Delete
 */
static cJSON *build_task_update(const char *id, const char *kind, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *status;
    cJSON *artifacts;
    cJSON *artifact;
    cJSON *parts;
    cJSON *part;

    if (root == RT_NULL)
    {
        return RT_NULL;
    }

    cJSON_AddStringToObject(root, "id", id);

    status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "state", "working");
    cJSON_AddItemToObject(root, "status", status);

    artifacts = cJSON_AddArrayToObject(root, "artifacts");
    artifact = cJSON_CreateObject();
    parts = cJSON_AddArrayToObject(artifact, "parts");
    part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(parts, part);
    /* name 标出这是哪类内容，append 表示与前面的内容续接 */
    cJSON_AddStringToObject(artifact, "name", kind);
    cJSON_AddBoolToObject(artifact, "append", 1);
    cJSON_AddItemToArray(artifacts, artifact);

    return root;
}

/*
 * @brief 把队列里的中间事件推给客户端
 * @param session webnet 会话
 * @param id      任务 id
 */
static void flush_stream_events(struct webnet_session *session, const char *id)
{
    agent_stream_event_t event;

    while (agent_stream_take(&event))
    {
        cJSON *update = build_task_update(id, agent_stream_kind_name(event.kind),
                                          event.text);
        if (update == RT_NULL)
        {
            continue;
        }
        char *encoded = cJSON_PrintUnformatted(update);
        cJSON_Delete(update);
        if (encoded != RT_NULL)
        {
            agent_wn_send_sse_event(session, RT_NULL, encoded);
            cJSON_free(encoded);
        }
    }
}

/*
 * @brief 等 agent 的最终结果
 * @param session webnet 会话，为 NULL 时不推中间事件
 * @param id      任务 id
 * @param out     回复文本，失败时为 NULL
 * @return RT_EOK 成功
 */
static rt_err_t task_wait(struct webnet_session *session, const char *id, char **out)
{
    Messages_t output;
    rt_tick_t start = rt_tick_get();
    rt_int32_t timeout_tick = rt_tick_from_millisecond(A2A_REPLY_TIMEOUT_MS);
    rt_int32_t poll_tick = rt_tick_from_millisecond(A2A_POLL_INTERVAL_MS);

    *out = RT_NULL;

    for (;;)
    {
        if (session != RT_NULL)
        {
            flush_stream_events(session, id);
        }

        output = message_hub_get_timeout(message_hub, message_hub->output_mailbox,
                                         poll_tick);
        if (output != RT_NULL)
        {
            const char *text = messages_get_content_idx(output, 0);

            if (text != RT_NULL)
            {
                *out = rt_strdup(text);
            }
            messages_destroy(output);
            return (*out != RT_NULL) ? RT_EOK : -RT_ENOMEM;
        }

        if ((rt_int32_t)(rt_tick_get() - start) > timeout_tick)
        {
            LOG_E("a2a task %s timed out", id);
            return -RT_ETIMEOUT;
        }
    }
}

/*
 * @brief GET /.well-known/agent.json，返回能力卡
 */
static void cgi_agent_card(struct webnet_session *session)
{
    cJSON *card;
    cJSON *caps;
    cJSON *inputs;
    cJSON *outputs;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        return;
    }

    card = cJSON_CreateObject();
    if (card == RT_NULL)
    {
        agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                  "error", "out of memory");
        return;
    }

    cJSON_AddStringToObject(card, "name", g_card.name);
    cJSON_AddStringToObject(card, "description", g_card.description);
    cJSON_AddStringToObject(card, "url",
                            agent_wn_safe_string(session->request->host));
    cJSON_AddStringToObject(card, "version", g_card.version);

    caps = cJSON_CreateObject();
    /* 同上，cJSON_False 是 1，要写 0 */
    cJSON_AddBoolToObject(caps, "streaming", 1);
    cJSON_AddBoolToObject(caps, "pushNotifications", 1);
    cJSON_AddBoolToObject(caps, "stateTransitionHistory", 1);
    cJSON_AddItemToObject(card, "capabilities", caps);

    inputs = cJSON_AddArrayToObject(card, "defaultInputModes");
    cJSON_AddItemToArray(inputs, cJSON_CreateString("text"));
    outputs = cJSON_AddArrayToObject(card, "defaultOutputModes");
    cJSON_AddItemToArray(outputs, cJSON_CreateString("text"));

    cJSON_AddItemToObject(card, "skills", cJSON_CreateArray());

    if (agent_wn_send_json_object(session, 200, "OK", card) != RT_EOK)
    {
        agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                  "error", "failed to serialize agent card");
    }
    cJSON_Delete(card);
}

/*
 * @brief GET /card 读当前能力卡，POST /card 改并落盘
 * @note  name 必填，description 与 version 不传就保持原值；
 *        改完写 SD 卡，重启后还是这份，Kconfig 里那两个值只当默认
 */
static void cgi_card(struct webnet_session *session)
{
    char *body;
    cJSON *root;
    cJSON *item;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        return;
    }

    if (session->request->method == WEBNET_GET)
    {
        root = cJSON_CreateObject();
        if (root == RT_NULL)
        {
            agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                      "error", "out of memory");
            return;
        }
        cJSON_AddStringToObject(root, "name", g_card.name);
        cJSON_AddStringToObject(root, "description", g_card.description);
        cJSON_AddStringToObject(root, "version", g_card.version);
        if (agent_wn_send_json_object(session, 200, "OK", root) != RT_EOK)
        {
            agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                      "error", "failed to serialize card");
        }
        cJSON_Delete(root);
        return;
    }

    if (session->request->method != WEBNET_POST)
    {
        agent_wn_send_simple_json(session, 405, "Method Not Allowed", RT_FALSE,
                                  "error", "use GET to read, POST to update");
        return;
    }

    body = agent_wn_copy_request_body(session, A2A_CARD_BODY_MAX);
    if (body == RT_NULL)
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "invalid or oversized request body");
        return;
    }

    root = cJSON_Parse(body);
    rt_free(body);
    if (root == RT_NULL || !cJSON_IsObject(root))
    {
        if (root != RT_NULL)
        {
            cJSON_Delete(root);
        }
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "body must be a JSON object");
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0')
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "name must be a non-empty string");
        goto cleanup;
    }
    if (rt_strlen(item->valuestring) >= sizeof(g_card.name))
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "name is too long");
        goto cleanup;
    }
    rt_snprintf(g_card.name, sizeof(g_card.name), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (item != RT_NULL)
    {
        if (!cJSON_IsString(item) ||
            rt_strlen(item->valuestring) >= sizeof(g_card.description))
        {
            agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                      "error", "description is too long");
            goto cleanup;
        }
        rt_snprintf(g_card.description, sizeof(g_card.description), "%s",
                    item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (item != RT_NULL)
    {
        if (!cJSON_IsString(item) || rt_strlen(item->valuestring) >= sizeof(g_card.version))
        {
            agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                      "error", "version is too long");
            goto cleanup;
        }
        rt_snprintf(g_card.version, sizeof(g_card.version), "%s", item->valuestring);
    }

    if (card_save() != RT_EOK)
    {
        agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                  "error", "failed to write the card file");
        goto cleanup;
    }

    LOG_I("agent card updated: name=%s, version=%s", g_card.name, g_card.version);
    agent_wn_send_simple_json(session, 200, "OK", RT_TRUE, "status", "ok");

cleanup:
    cJSON_Delete(root);
}

/*
 * @brief 造一个任务结果对象
 * @param id      任务 id
 * @param state   状态：working / completed / failed
 * @param reply   回复文本，可为 NULL
 * @return 任务结果，调用方负责 cJSON_Delete
 */
static cJSON *build_task_response(const char *id, const char *state, const char *reply)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *status;
    cJSON *artifacts;
    cJSON *artifact;
    cJSON *parts;
    cJSON *part;

    if (root == RT_NULL)
    {
        return RT_NULL;
    }

    cJSON_AddStringToObject(root, "id", id);

    status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "state", state);
    cJSON_AddItemToObject(root, "status", status);

    artifacts = cJSON_AddArrayToObject(root, "artifacts");
    if (reply != RT_NULL)
    {
        artifact = cJSON_CreateObject();
        parts = cJSON_AddArrayToObject(artifact, "parts");
        part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "type", "text");
        cJSON_AddStringToObject(part, "text", reply);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddStringToObject(artifact, "name", "response");
        cJSON_AddItemToArray(artifacts, artifact);
    }
    return root;
}

/*
 * @brief 把任务投给 agent，把结果按 JSON 或 SSE 回出去
 * @param session webnet 会话
 * @param stream  为真时按 SSE 分帧
 */
static void handle_task(struct webnet_session *session, rt_bool_t stream)
{
    char *body;
    char *text;
    char *reply = RT_NULL;
    const char *id;
    cJSON *response;
    char *encoded;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        return;
    }
    if (session->request->method != WEBNET_POST)
    {
        agent_wn_send_simple_json(session, 405, "Method Not Allowed", RT_FALSE,
                                  "error", "use POST");
        return;
    }

    body = agent_wn_copy_request_body(session, A2A_REQUEST_MAX_SIZE);
    if (body == RT_NULL)
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "invalid or oversized request body");
        return;
    }

    text = extract_text(body);
    rt_free(body);
    if (text == RT_NULL)
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "message.parts[0].text must be a non-empty string");
        return;
    }

    id = next_task_id();

    /* 新任务开始，清掉上一轮可能残留的中间事件 */
    agent_stream_clear();

    if (stream)
    {
        /* 先推一帧 working，再等最终结果，让客户端看到任务确实在跑 */
        session->request->result_code = 200;
        webnet_session_set_header(session, "text/event-stream", 200, "OK", -1);
        response = build_task_response(id, "working", RT_NULL);
        if (response != RT_NULL)
        {
            encoded = cJSON_PrintUnformatted(response);
            if (encoded != RT_NULL)
            {
                agent_wn_send_sse_event(session, RT_NULL, encoded);
                cJSON_free(encoded);
            }
            cJSON_Delete(response);
        }
    }

    if (task_submit(text) != RT_EOK)
    {
        LOG_E("a2a task %s submit failed", id);
        response = build_task_response(id, "failed", RT_NULL);
        if (response != RT_NULL)
        {
            encoded = cJSON_PrintUnformatted(response);
            if (encoded != RT_NULL)
            {
                if (stream)
                {
                    agent_wn_send_sse_event(session, RT_NULL, encoded);
                }
                else
                {
                    agent_wn_send_raw_json(session, 502, "Bad Gateway", encoded);
                }
                cJSON_free(encoded);
            }
            cJSON_Delete(response);
        }
        rt_free(text);
        return;
    }

    if (task_wait(stream ? session : RT_NULL, id, &reply) != RT_EOK)
    {
        LOG_E("a2a task %s failed", id);
        response = build_task_response(id, "failed", RT_NULL);
        if (response != RT_NULL)
        {
            encoded = cJSON_PrintUnformatted(response);
            if (encoded != RT_NULL)
            {
                if (stream)
                {
                    agent_wn_send_sse_event(session, RT_NULL, encoded);
                }
                else
                {
                    agent_wn_send_raw_json(session, 502, "Bad Gateway", encoded);
                }
                cJSON_free(encoded);
            }
            cJSON_Delete(response);
        }
        rt_free(text);
        return;
    }

    response = build_task_response(id, "completed", reply);
    rt_free(text);
    rt_free(reply);
    if (response == RT_NULL)
    {
        agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                  "error", "out of memory");
        return;
    }

    encoded = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (encoded == RT_NULL)
    {
        agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                  "error", "failed to serialize task result");
        return;
    }

    if (stream)
    {
        agent_wn_send_sse_event(session, RT_NULL, encoded);
    }
    else
    {
        agent_wn_send_raw_json(session, 200, "OK", encoded);
    }
    cJSON_free(encoded);
}

/*
 * @brief POST /tasks/send，同步等最终结果
 */
static void cgi_task_send(struct webnet_session *session)
{
    handle_task(session, RT_FALSE);
}

/*
 * @brief POST /tasks/sendSubscribe，SSE 推状态与结果
 */
static void cgi_task_stream(struct webnet_session *session)
{
    handle_task(session, RT_TRUE);
}

/*
 * @brief POST /tasks/pushNotificationConfig，下发上报回调地址
 * @note  请求体形如 {"url":"http://192.168.137.1:8080/api/devices/push/xxx","id":"dev-1"}
 *        url 传空串表示关闭上报
 */
static void cgi_push_config(struct webnet_session *session)
{
    char *body;
    cJSON *root;
    cJSON *item;
    const char *url;
    const char *id;

    if (session == RT_NULL || session->request == RT_NULL)
    {
        return;
    }

    if (session->request->method == WEBNET_GET)
    {
        root = cJSON_CreateObject();
        if (root == RT_NULL)
        {
            agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                      "error", "out of memory");
            return;
        }
        cJSON_AddStringToObject(root, "url", agent_report_callback());
        cJSON_AddStringToObject(root, "id", agent_report_callback_id());
        if (agent_wn_send_json_object(session, 200, "OK", root) != RT_EOK)
        {
            agent_wn_send_simple_json(session, 500, "Internal Server Error", RT_FALSE,
                                      "error", "failed to serialize the push config");
        }
        cJSON_Delete(root);
        return;
    }

    if (session->request->method != WEBNET_POST)
    {
        agent_wn_send_simple_json(session, 405, "Method Not Allowed", RT_FALSE,
                                  "error", "use GET to read, POST to update");
        return;
    }

    body = agent_wn_copy_request_body(session, A2A_REQUEST_MAX_SIZE);
    if (body == RT_NULL)
    {
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "invalid or oversized request body");
        return;
    }

    root = cJSON_Parse(body);
    rt_free(body);
    if (root == RT_NULL || !cJSON_IsObject(root))
    {
        if (root != RT_NULL)
        {
            cJSON_Delete(root);
        }
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "body must be a JSON object");
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(item))
    {
        cJSON_Delete(root);
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "url must be a string");
        return;
    }
    url = item->valuestring;

    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    id = cJSON_IsString(item) ? item->valuestring : RT_NULL;

    if (agent_report_set_callback(url, id) != RT_EOK)
    {
        cJSON_Delete(root);
        agent_wn_send_simple_json(session, 400, "Bad Request", RT_FALSE,
                                  "error", "the push callback url is invalid");
        return;
    }

    cJSON_Delete(root);
    LOG_I("push notification config updated");
    agent_wn_send_simple_json(session, 200, "OK", RT_TRUE, "status", "ok");
}

/*
 * @brief A2A 通道初始化
 * @param hub 消息中心句柄
 * @param ctx 上下文管理器句柄
 */
void a2a_agent_channel_init(MessageHub_t hub, Context_t ctx)
{
    if (hub == RT_NULL || ctx == RT_NULL)
    {
        LOG_E("A2A channel requires a valid message hub and context");
        return;
    }

    message_hub = hub;
    context = ctx;

    if (started)
    {
        LOG_W("A2A channel is already started");
        return;
    }

    /* 先读上次下发的能力卡，再注册 CGI，避免第一帧能力卡还是默认值 */
    card_load();

    /* CGI 根切到 "/"，否则 webnet 只认包含 /cgi-bin/ 的路径，
       /.well-known/agent.json 和 /tasks/send 都匹配不上 */
    webnet_cgi_set_root("/");
    webnet_cgi_register(".well-known/agent.json", cgi_agent_card);
    webnet_cgi_register("card", cgi_card);
    webnet_cgi_register("tasks/send", cgi_task_send);
    webnet_cgi_register("tasks/sendSubscribe", cgi_task_stream);
    webnet_cgi_register("tasks/pushNotificationConfig", cgi_push_config);

    webnet_init();
    started = RT_TRUE;

    LOG_I("A2A channel up on port %d", WEBNET_PORT);
}
