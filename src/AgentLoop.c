#include "chat.h"
#include "context.h"
#include "MessageHub.h"
#include "tool_func.h"
#include "utils.h"
#include "AgentChannels.h"
#include "AgentRuntime.h"

#define LOG_TAG "Agent.AgentLoop"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;
static rt_thread_t g_main_agent_loop = RT_NULL;
static rt_bool_t g_agent_running = RT_FALSE;
/* agent 是否正在处理一轮对话：供外部注入方（如 MQTT 接收线程）错峰投递 */
static volatile rt_bool_t g_agent_busy = RT_FALSE;
/* 通道 ops 函数指针：init/reset 统一经此分发，宏选择只在 AgentChannels.h 一处 */
static const AgentChannelOps *g_channel_ops = RT_NULL;

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
static rt_thread_t g_file_op_thread = RT_NULL;
#endif

static void cleanup_agent(void);
static rt_sem_t g_cleanup_sem = RT_NULL;

/* ==========================================================================
 * 工具循环防呆：重复调用检测
 *   模型有时会反复调用同一个工具（参数相同或只改 max 之类的无关参数），
 *   如果工具返回空结果就会无限重试。这里记录最近几次调用签名，重复的直接跳过
 *   执行并把「结果不会变」告诉模型；连续整轮重复则强制生成文本回答。
 * ========================================================================== */
#define AGENT_CALL_SIG_LEN   192
#define AGENT_CALL_SIG_KEEP  6

static char g_call_sig[AGENT_CALL_SIG_KEEP][AGENT_CALL_SIG_LEN];
static int g_call_sig_count = 0;

static void agent_call_sig_reset(void)
{
    g_call_sig_count = 0;
}

static rt_bool_t agent_call_sig_seen(const char *sig)
{
    int i;

    for (i = 0; i < g_call_sig_count; i++)
    {
        if (rt_strcmp(g_call_sig[i], sig) == 0)
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void agent_call_sig_add(const char *sig)
{
    if (g_call_sig_count < AGENT_CALL_SIG_KEEP)
    {
        rt_strncpy(g_call_sig[g_call_sig_count], sig, AGENT_CALL_SIG_LEN - 1);
        g_call_sig[g_call_sig_count][AGENT_CALL_SIG_LEN - 1] = '\0';
        g_call_sig_count++;
        return;
    }

    /* 满了就整体前移，丢弃最旧的一条 */
    rt_memmove(g_call_sig[0], g_call_sig[1], (AGENT_CALL_SIG_KEEP - 1) * AGENT_CALL_SIG_LEN);
    rt_strncpy(g_call_sig[AGENT_CALL_SIG_KEEP - 1], sig, AGENT_CALL_SIG_LEN - 1);
    g_call_sig[AGENT_CALL_SIG_KEEP - 1][AGENT_CALL_SIG_LEN - 1] = '\0';
}

/*
 * 同一轮对话里同一个工具被反复调用（参数在变，例如 max=8 → max=10）时，
 * 精确签名比对抓不住。这里再按「工具名」计数，超过上限就视为无效重复，
 * 触发和重复调用一样的收尾逻辑，避免模型换个参数无限试下去。
 */
#define AGENT_TOOL_NAME_LEN  32
#define AGENT_TOOL_HITS_MAX  3

static char g_tool_hits_name[AGENT_CALL_SIG_KEEP][AGENT_TOOL_NAME_LEN];
static int g_tool_hits_count[AGENT_CALL_SIG_KEEP];
static int g_tool_hits_used = 0;

static void agent_tool_hits_reset(void)
{
    g_tool_hits_used = 0;
    rt_memset(g_tool_hits_count, 0, sizeof(g_tool_hits_count));
}

/* 记录一次调用并返回该工具在本轮对话中已被调用的次数（含本次） */
static int agent_tool_hit(const char *name)
{
    int i;

    for (i = 0; i < g_tool_hits_used; i++)
    {
        if (rt_strcmp(g_tool_hits_name[i], name) == 0)
        {
            return ++g_tool_hits_count[i];
        }
    }

    if (g_tool_hits_used < AGENT_CALL_SIG_KEEP)
    {
        rt_strncpy(g_tool_hits_name[g_tool_hits_used], name, AGENT_TOOL_NAME_LEN - 1);
        g_tool_hits_name[g_tool_hits_used][AGENT_TOOL_NAME_LEN - 1] = '\0';
        g_tool_hits_count[g_tool_hits_used] = 1;
        g_tool_hits_used++;
    }
    return 1;
}

/*
 * @brief 向输出通道投递一段纯文本（工具循环兜底、空回答兜底用）
 * @param text 文本内容，RT_NULL/空串时用默认提示
 */
static void agent_put_text(const char *text)
{
    Messages_t messages;

    if (text == RT_NULL || text[0] == '\0')
    {
        text = "(no answer generated)";
    }

    messages = messages_create(1);
    if (messages == RT_NULL)
    {
        return;
    }
    if (messages_append(messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(messages);
        return;
    }
    if (g_message_hub->put_message(g_message_hub, messages, g_message_hub->output_mailbox) != RT_EOK)
    {
        LOG_W("output mailbox full, drop message");
        messages_destroy(messages);
    }
}

/*
 * @brief 兜底提示：既投递到输出 mailbox 供通道消费，也走流式打印路径
 * @note  CLI 通道不打印 mailbox 里的内容，只打印流式 on_context（见 channels/CLI.c），
 *        所以「请求失败」「工具循环超限」这类模型不会流式输出的文本必须两边都发
 */
static void agent_notify(const char *text, void (*on_context)(const char *text))
{
    agent_put_text(text);
    if (on_context != RT_NULL)
    {
        on_context(text);
    }
}

/*
 * @brief 把一次 LLM 回复作为最终回答输出（无 tool_calls 或强制收尾时使用）
 * @param resp       LLM 回复
 * @param on_context 正文打印回调（流式路径，通道可见）
 */
static void agent_emit_answer(ChatResponse_t resp, void (*on_context)(const char *text))
{
    if (resp == RT_NULL)
    {
        return;
    }

    g_context->append_assistant_message(g_context, resp);

    if (resp->context == RT_NULL || resp->context[0] == '\0')
    {
        /*
         * 模型这次既没有工具调用也没有正文：必须给用户一句话，
         * 而且要走流式打印路径——CLI 通道收到输出 mailbox 的消息后只打印换行、
         * 不打印内容（见 channels/CLI.c 的 CLI_run），只投 mailbox 用户是看不到的。
         */
        agent_notify("(the model returned no answer; please restate your question)", on_context);
    }
    else
    {
        /* 正文已通过 on_context 流式打印过，这里只投 mailbox 供其它通道消费 */
        agent_put_text(resp->context);
    }
}

/*
 * @brief 获取当前 agent 消息中心句柄
 * @note  供外部模块向 agent 主动注入消息（如 MQTT 订阅回调）使用；
 *        agent 未运行时返回 RT_NULL，调用方需判空
 * @return 消息中心句柄
 */
MessageHub_t agent_get_message_hub(void)
{
    return g_message_hub;
}

/*
 * @brief 查询 agent 主循环是否正在运行
 * @return RT_TRUE 运行中，RT_FALSE 未运行
 */
rt_bool_t agent_is_running(void)
{
    return g_agent_running;
}

/*
 * @brief 查询 agent 是否正在处理一轮对话（含 LLM 请求与工具执行）
 * @return RT_TRUE 忙，RT_FALSE 空闲（阻塞等待新消息中）
 */
rt_bool_t agent_is_busy(void)
{
    return g_agent_busy;
}

/*
 * @brief 初始化 agent（工具、消息中心、上下文、通道）
 */
static void init_agent(void)
{
    g_channel_ops = AGENT_CHANNEL_OPS;

    init_tools();
    g_message_hub = message_hub_create();
    g_context = agent_context_create();

    if (g_message_hub == RT_NULL || g_context == RT_NULL)
    {
        LOG_E("init_agent: message_hub/context create failed");
        return;
    }

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
    g_file_op_thread = agent_file_op_init();
#endif

    if (g_channel_ops != RT_NULL && g_channel_ops->init != RT_NULL)
    {
        g_channel_ops->init(g_message_hub, g_context);
    }

#ifdef RT_USING_HEAP
    {
        rt_size_t heap_total = 0, heap_used = 0, heap_max_used = 0;
        rt_memory_info(&heap_total, &heap_used, &heap_max_used);
        LOG_I("[mem] agent started: heap total=%d, used=%d, max_used=%d",
              (int)heap_total, (int)heap_used, (int)heap_max_used);
    }
#endif
}

/*
 * @brief Agent 工具调用循环（最大 loop_max 轮）
 * @param g_context     上下文管理器
 * @param tools         工具定义 JSON 数组
 * @param on_reasoning  思考过程回调
 * @param on_tool_call  工具调用回调
 * @param on_context    回复内容回调
 */
static void agent_loop(Context_t g_context,
                       cJSON *tools,
                       void (*on_reasoning)(const char *text),
                       void (*on_tool_call)(const char *text),
                       void (*on_context)(const char *text))
{
    const int loop_max = 6;
    const int chat_retry_max = 3;
    int loop_cnt = 0;
    int attempt = 0;
    int dup_rounds = 0;
    rt_bool_t answered = RT_FALSE;
    ChatResponse_t resp = RT_NULL;

    agent_call_sig_reset();
    agent_tool_hits_reset();

    while (loop_cnt < loop_max)
    {
        loop_cnt++;

        /* 请求失败重试：TLS/网络抖动（如 mbedtls NET_RECV_FAILED、对端重置）不应直接
           丢弃整轮对话；已流式输出的部分内容在重试时可能重复打印，属可接受的代价 */
        for (attempt = 1; attempt <= chat_retry_max; attempt++)
        {
            resp = chat(g_context->message, tools, 32 * 1024, on_reasoning, on_tool_call, on_context);
            if (resp != RT_NULL)
            {
                break;
            }
            if (attempt < chat_retry_max)
            {
                LOG_W("Chat request failed (attempt %d/%d), retry in %d ms",
                      attempt, chat_retry_max, 1000 * attempt);
                rt_thread_mdelay(1000 * attempt);
            }
        }

        if (resp == RT_NULL)
        {
            LOG_E("Chat request failed after %d attempts, terminating this case", chat_retry_max);
            agent_notify("(LLM request failed after retries, please try again)", on_context);
            answered = RT_TRUE;
            break;
        }

        /* 无 tool_calls：最终回答 */
        if (cJSON_GetArraySize(resp->tool_call) == 0)
        {
            agent_emit_answer(resp, on_context);
            chat_response_free(resp);
            resp = RT_NULL;
            answered = RT_TRUE;
            break;
        }

        /* 执行所有 tool_calls */
        int tool_count = cJSON_GetArraySize(resp->tool_call);
        int dup_hits = 0;
        int over_hits = 0;
        LOG_I("\nDetected %d tool invocation(s)\n", tool_count);
        for (int t = 0; t < tool_count; t++)
        {
            cJSON *tc_item = cJSON_GetArrayItem(resp->tool_call, t);
            AgentToolNode_t tool_node = search_agent_tool_node(tc_item);

            if (!tc_item || !tool_node) continue;

            cJSON *tc_func = cJSON_GetObjectItemCaseSensitive(tc_item, "function");
            if (!tc_func || !cJSON_IsObject(tc_func))
            {
                LOG_I("Tool call %d missing function field, skip\n", t);
                continue;
            }

            cJSON *name_node = cJSON_GetObjectItemCaseSensitive(tc_func, "name");
            cJSON *args_node = cJSON_GetObjectItemCaseSensitive(tc_func, "arguments");
            cJSON *id_node = cJSON_GetObjectItem(tc_item, "id");
            if (!name_node || !cJSON_IsString(name_node) || !args_node || !cJSON_IsString(args_node))
            {
                LOG_I("Tool call %d invalid name/arguments, skip\n", t);
                continue;
            }

            const char *func_name = name_node->valuestring;
            const char *args_str = args_node->valuestring;
            /* id 缺失/非字符串时兜底，保证 append_tool_message 的 tool_call_id 有效 */
            const char *id_str = (id_node && cJSON_IsString(id_node)) ? id_node->valuestring : "call_0";

            /*
             * 重复调用检测：同一轮对话里已经用完全相同的参数调用过同一个工具，
             * 结果不会变化。此时不再执行，直接把「结果不变、请直接回答」喂回模型，
             * 否则一旦工具返回内容为空，模型会无限重试同一个调用。
             */
            char sig[AGENT_CALL_SIG_LEN];
            rt_snprintf(sig, sizeof(sig), "%s|%s", func_name, args_str);
            if (agent_call_sig_seen(sig))
            {
                LOG_W("duplicated tool call skipped: %s(%s)", func_name, args_str);
                dup_hits++;

                Messages_t dup_messages = messages_create(1);
                if (dup_messages != RT_NULL)
                {
                    if (messages_append(dup_messages, TYPE_TEXT,
                                        "This exact tool call was already executed in this "
                                        "conversation and its result is unchanged (see the "
                                        "previous tool result). Do not call it again - answer "
                                        "the user with the information you already have.") != RT_EOK)
                    {
                        messages_destroy(dup_messages);
                        dup_messages = RT_NULL;
                    }
                }
                if (dup_messages != RT_NULL)
                {
                    /* append_tool_message 内部是拷贝（见 context.c 的 to_content），
                       这里必须自行释放，否则每次重复调用都会泄漏一个消息列表 */
                    g_context->append_tool_message(g_context, id_str, dup_messages);
                    messages_destroy(dup_messages);
                }
                continue;
            }
            agent_call_sig_add(sig);

            /* 同一工具被反复调用（哪怕参数在变）：超过上限就计入无效轮次 */
            if (agent_tool_hit(func_name) > AGENT_TOOL_HITS_MAX)
            {
                LOG_W("tool %s called too many times in this case, treating as unproductive",
                      func_name);
                over_hits++;
            }

            cJSON *args_json = cJSON_Parse(args_str);
            if (!args_json)
            {
                LOG_I("Tool %s parse arguments failed, skip execution\n", func_name);
                continue;
            }

            tool_node->execute_func(args_json, tool_node);

            Messages_t tool_ret_messages = tool_node->ret.messages;
            /* 工具参数校验失败时 ret.messages 可能为 NULL */
            if (tool_ret_messages == RT_NULL)
            {
                cJSON_Delete(args_json);
                continue;
            }
            for (int i = 0; i < tool_ret_messages->current_size; i++)
            {
                LOG_I("[Local Tool %s Execution Result] type %s, result %s\n", func_name,
                      get_agent_content_type(messages_get_type_idx(tool_ret_messages, i)),
                      messages_get_content_idx(tool_ret_messages, i));
            }

            g_context->append_tool_message(g_context, id_str, tool_ret_messages);

            cJSON_Delete(args_json);
        }

        /*
         * 整轮都是重复调用（或同一工具反复调用到了上限）：说明模型不认可已有结果。
         * 连续两轮如此就强制它用文本回答，避免「一直卡在工具调用」而不给用户任何回复。
         */
        if (tool_count > 0 && (dup_hits + over_hits) >= tool_count)
        {
            dup_rounds++;
            if (dup_rounds >= 2)
            {
                LOG_W("repeated tool calls detected, forcing a final answer");
                chat_response_free(resp);
                resp = RT_NULL;
                break;
            }
        }
        else
        {
            dup_rounds = 0;
        }

        chat_response_free(resp);
        resp = RT_NULL;
    }

    if (loop_cnt >= loop_max)
    {
        LOG_E("Maximum tool loop count %d reached, forcing a final answer\n", loop_max);
        chat_response_free(resp);
        resp = RT_NULL;
    }

    /*
     * 收尾：仍然没有给用户任何回答时（达到轮次上限、或检测到重复调用而跳出），
     * 最后一次不带工具询问模型，逼出一段纯文本回答；失败就给一句明确的提示，
     * 保证这一轮对话不会「既没有回答也没有结束」。
     * 注意必须用 answered 判断：正常出回答的路径 resp 也是 NULL，
     * 只按 resp==NULL 判断会再问一次、重复输出一遍回答。
     */
    if (!answered)
    {
        resp = chat(g_context->message, RT_NULL, 32 * 1024, on_reasoning, RT_NULL, on_context);
        if (resp != RT_NULL)
        {
            agent_emit_answer(resp, on_context);
            chat_response_free(resp);
            resp = RT_NULL;
        }
        else
        {
            LOG_E("forced final answer failed");
            agent_notify("(stopped after too many tool calls without a usable result - "
                         "please restate your question)", on_context);
        }
    }
}

/*
 * @brief 主循环线程：接收消息 -> 调用 LLM -> 执行工具 -> 返回结果
 * @param param 线程参数（未使用）
 */
static void main_loop(void *param)
{
    init_agent();
    /* init_agent 失败（hub/context 创建失败）时直接退出并清理 */
    if (g_message_hub == RT_NULL || g_context == RT_NULL)
    {
        cleanup_agent();
        return;
    }

    g_agent_running = RT_TRUE;
    while (g_agent_running)
    {
        Messages_t messages = g_message_hub->get_message(g_message_hub, g_message_hub->input_mailbox);
        /* RT_NULL 为 cleanup_agent 发出的唤醒信号 */
        if (messages == RT_NULL)
        {
            continue;
        }

        g_context->append_user_message(g_context, messages);
        /* 输入消息所有权在 main_loop（消费者），使用完毕后释放 */
        messages_destroy(messages);

        g_agent_busy = RT_TRUE;
        agent_loop(g_context, get_agent_tools(), print_reasoning, print_tool_call, print_context);
        g_agent_busy = RT_FALSE;

        /* 裁剪上下文，防止无限膨胀 */
        if (g_context->trim_context)
        {
            g_context->trim_context(g_context, PKG_AGENT_MESSAGE_TRIM);
        }
    }
    cleanup_agent();
}

/*
 * @brief 主循环入口（MSH 命令）
 * @return 0 成功，1 失败
 */
static int main_loop_entry(void)
{
    if (g_main_agent_loop != RT_NULL)
    {
        LOG_W("Agent main loop already running");
        return 0;
    }
    g_main_agent_loop = rt_thread_create("AgentLoop", main_loop, RT_NULL, 10240, 10, 10);
    if (!g_main_agent_loop)
    {
        LOG_E("g_main_agent_loop thread create failed");
        return 1;
    }
    rt_thread_startup(g_main_agent_loop);
    return 0;
}

/*
 * @brief 发送停止信号唤醒主循环退出
 */
static void signal_agent_stop(void)
{
    g_agent_running = RT_FALSE;
    if (g_message_hub != RT_NULL && g_message_hub->input_mailbox != RT_NULL)
    {
        /* 使用阻塞发送保证唤醒消息一定能进入 mailbox */
        rt_mb_send_wait(g_message_hub->input_mailbox, (rt_ubase_t)RT_NULL, RT_WAITING_FOREVER);
    }
}

/*
 * @brief 清理 agent 资源（通道、消息中心、上下文、工具、多模态线程）
 */
static void cleanup_agent(void)
{
    /* 1. 先停止通道，防止其继续访问即将销毁的 message_hub（幂等） */
    if (g_channel_ops != RT_NULL && g_channel_ops->reset != RT_NULL)
    {
        g_channel_ops->reset();
    }

    /* 2. 向输出 mailbox 发 NULL 哨兵，唤醒仍可能阻塞在输出等待的通道 */
    if (g_message_hub != RT_NULL && g_message_hub->output_mailbox != RT_NULL)
    {
        rt_mb_send(g_message_hub->output_mailbox, (rt_ubase_t)RT_NULL);
    }

    /* 3. 销毁消息中心（内部会排空残留消息）
       先摘除全局引用再销毁：避免外部注入方（如 MQTT 订阅线程）拿到即将销毁的句柄 */
    if (g_message_hub)
    {
        MessageHub_t hub = g_message_hub;
        g_message_hub = RT_NULL;
        message_hub_destroy(hub);
    }

    /* 4. 销毁上下文 */
    if (g_context)
    {
        agent_context_destroy(g_context);
        g_context = RT_NULL;
    }

    /* 5. 清理工具系统（释放工具链表与残留的工具结果消息），再次进入时重新注册 */
    agent_tools_cleanup();

    g_main_agent_loop = RT_NULL;
    g_agent_running = RT_FALSE;
    g_agent_busy = RT_FALSE;
    g_channel_ops = RT_NULL;
#ifdef PKG_AGENT_MULTIMODAL_ENABLE
    if (g_file_op_thread)
    {
        agent_file_op_deinit();
    }
    g_file_op_thread = RT_NULL;
#endif

#ifdef RT_USING_HEAP
    {
        rt_size_t heap_total = 0, heap_used = 0, heap_max_used = 0;
        rt_memory_info(&heap_total, &heap_used, &heap_max_used);
        LOG_I("[mem] cleanup done: heap total=%d, used=%d, max_used=%d",
              (int)heap_total, (int)heap_used, (int)heap_max_used);
    }
#endif

    /* 通知清理完成 */
    if (g_cleanup_sem != RT_NULL)
    {
        rt_sem_release(g_cleanup_sem);
    }
}

/*
 * @brief 清理 agent 入口（MSH 命令）
 * @return 0 成功
 */
static int cleanup_agent_entry(void)
{
    if (g_main_agent_loop == RT_NULL)
    {
        LOG_W("Agent not running, nothing to clean up");
        return 0;
    }

    /* 创建完成信号量 */
    if (g_cleanup_sem == RT_NULL)
    {
        g_cleanup_sem = rt_sem_create("agent_exit", 0, RT_IPC_FLAG_FIFO);
        if (g_cleanup_sem == RT_NULL)
        {
            LOG_E("Failed to create cleanup semaphore");
            return -RT_ERROR;
        }
    }

    /* 先停止通道线程，防止它们继续访问即将销毁的 message_hub */
    if (g_channel_ops != RT_NULL && g_channel_ops->reset != RT_NULL)
    {
        g_channel_ops->reset();
    }

    LOG_I("Signaling agent to stop...");
    signal_agent_stop();

    /* 等待主循环线程真正退出 */
    rt_sem_take(g_cleanup_sem, RT_WAITING_FOREVER);
    rt_sem_delete(g_cleanup_sem);
    g_cleanup_sem = RT_NULL;

    LOG_I("Agent stopped successfully.");
    return 0;
}

MSH_CMD_EXPORT(main_loop_entry, main_loop_entry);
MSH_CMD_EXPORT(cleanup_agent_entry, cleanup_agent_entry);