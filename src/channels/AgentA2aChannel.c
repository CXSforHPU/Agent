#include "AgentA2aChannel.h"
#include "webnet_util.h"

#define LOG_TAG "Agent.channel.a2a"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define A2A_REQUEST_MAX_SIZE 4096
#define A2A_REPLY_TIMEOUT_MS 30000

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
 * @brief 把用户消息投给 agent 并等回复
 * @param  text 用户输入
 * @param  out  回复文本，失败时为 NULL
 * @return RT_EOK 成功
 */
static rt_err_t run_task(const char *text, char **out)
{
    Messages_t input;
    Messages_t output;
    rt_err_t result;

    *out = RT_NULL;
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
    input = RT_NULL;

    output = message_hub_get_timeout(message_hub, message_hub->output_mailbox,
                                     rt_tick_from_millisecond(A2A_REPLY_TIMEOUT_MS));
    if (output == RT_NULL || messages_get_content_idx(output, 0) == RT_NULL)
    {
        if (output != RT_NULL)
        {
            messages_destroy(output);
        }
        return -RT_ERROR;
    }

    *out = rt_strdup(messages_get_content_idx(output, 0));
    messages_destroy(output);
    result = (*out != RT_NULL) ? RT_EOK : -RT_ENOMEM;
    return result;
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

    cJSON_AddStringToObject(card, "name", PKG_AGENT_A2A_NAME);
    cJSON_AddStringToObject(card, "description", PKG_AGENT_A2A_DESCRIPTION);
    cJSON_AddStringToObject(card, "url",
                            agent_wn_safe_string(session->request->host));
    cJSON_AddStringToObject(card, "version", "1.0.0");

    caps = cJSON_CreateObject();
    /* 同上，cJSON_False 是 1，要写 0 */
    cJSON_AddBoolToObject(caps, "streaming", 1);
    cJSON_AddBoolToObject(caps, "pushNotifications", 0);
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

    if (run_task(text, &reply) != RT_EOK)
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

    /* CGI 根切到 "/"，否则 webnet 只认包含 /cgi-bin/ 的路径，
       /.well-known/agent.json 和 /tasks/send 都匹配不上 */
    webnet_cgi_set_root("/");
    webnet_cgi_register(".well-known/agent.json", cgi_agent_card);
    webnet_cgi_register("tasks/send", cgi_task_send);
    webnet_cgi_register("tasks/sendSubscribe", cgi_task_stream);

    webnet_init();
    started = RT_TRUE;

    LOG_I("A2A channel up on port %d", WEBNET_PORT);
}
