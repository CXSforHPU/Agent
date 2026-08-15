#include "chat.h"
#include "context.h"
#include "MessageHub.h"
#include "tool_func.h"
#include "utils.h"
#include "AgentChannels.h"

#define LOG_TAG "Agent.AgentLoop"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;
static rt_thread_t g_main_agent_loop = RT_NULL;
static rt_bool_t g_agent_running = RT_FALSE;
/* 通道 ops 函数指针：init/reset 统一经此分发，宏选择只在 AgentChannels.h 一处 */
static const AgentChannelOps *g_channel_ops = RT_NULL;

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
static rt_thread_t g_file_op_thread = RT_NULL;
#endif

static void cleanup_agent(void);
static rt_sem_t g_cleanup_sem = RT_NULL;

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
    const int loop_max = 10;
    int loop_cnt = 0;
    ChatResponse_t resp = RT_NULL;
    Messages_t assistant_messages = RT_NULL;

    while (loop_cnt < loop_max)
    {
        loop_cnt++;
        resp = chat(g_context->message, tools, 32 * 1024, on_reasoning, on_tool_call, on_context);
        if (resp == RT_NULL)
        {
            LOG_E("Chat request failed, terminating this case");
            break;
        }

        /* 无 tool_calls：最终回答 */
        if (cJSON_GetArraySize(resp->tool_call) == 0)
        {
            g_context->append_assistant_message(g_context, resp);

            assistant_messages = messages_create(1);
            messages_append(assistant_messages, TYPE_TEXT, resp->context);
            if (assistant_messages)
            {
                g_message_hub->put_message(g_message_hub, assistant_messages, g_message_hub->output_mailbox);
            }
            chat_response_free(resp);
            resp = RT_NULL;
            break;
        }

        /* 执行所有 tool_calls */
        int tool_count = cJSON_GetArraySize(resp->tool_call);
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
            const char *id_str = id_node->valuestring;

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
        chat_response_free(resp);
        resp = RT_NULL;
    }

    if (loop_cnt >= loop_max)
    {
        LOG_E("Maximum tool loop count %d reached, forced termination\n", loop_max);
        chat_response_free(resp);
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

        agent_loop(g_context, get_agent_tools(), print_reasoning, print_tool_call, print_context);

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

    /* 3. 销毁消息中心（内部会排空残留消息） */
    if (g_message_hub)
    {
        message_hub_destroy(g_message_hub);
        g_message_hub = RT_NULL;
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