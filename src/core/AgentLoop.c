#include "chat.h"
#include "context.h"
#include "memory.h"
#include "MessageHub.h"
#include "tool_func.h"
#include "utils.h"
#include "AgentChannels.h"
#include "AgentRuntime.h"
#include "AgentFsm.h"
#include "AgentStream.h"
#include "AgentOutput.h"

#define LOG_TAG "Agent.AgentLoop"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;
static rt_thread_t g_main_agent_loop = RT_NULL;
static rt_bool_t g_agent_running = RT_FALSE;
/* agent 是否正在处理一轮对话：供外部注入方（如 MQTT 接收线程）错峰投递 */
static volatile rt_bool_t g_agent_busy = RT_FALSE;
/* 当前这一轮是不是外部注入发起的：自发轮次的答案走上报通道 */
static volatile rt_bool_t g_turn_spontaneous = RT_FALSE;
/* 通道 ops 函数指针：init/reset 统一经此分发，宏选择只在 AgentChannels.h 一处 */
static const AgentChannelOps *g_channel_ops = RT_NULL;

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
static rt_thread_t g_file_op_thread = RT_NULL;
#endif

static void cleanup_agent(void);
static rt_sem_t g_cleanup_sem = RT_NULL;

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
 * @brief 查询当前这一轮是不是外部注入发起的
 * @return RT_TRUE 自发轮次，RT_FALSE 通道请求发起的轮次
 * @note  自发轮次（MQTT 告警、传感器事件等）没有人等它的回答，
 *        答案要走上报通道，不能投进 output mailbox
 */
rt_bool_t agent_turn_is_spontaneous(void)
{
    return g_turn_spontaneous;
}

/*
 * @brief 初始化 agent（工具、消息中心、上下文、通道）
 */
static void init_agent(void)
{
    g_channel_ops = AGENT_CHANNEL_OPS;

    /* 先定下外部服务器地址，MCP 客户端握手要用 */
    agent_endpoint_init();

    /* 流式事件队列：通道侧边等结果边把思考、工具、正文推给客户端 */
    agent_stream_init();

    /* 自发结论上报通道：读回控制台下发的回调地址 */
    agent_report_init();

    init_tools();
    agent_memory_init();
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

#ifdef PKG_AGENT_TTS_AUTO_PLAY
    agent_tts_speak_init();
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
 * @brief Agent 工具调用循环（状态机驱动，最大 loop_max 轮）
 * @note  这里只组装全局状态并启动状态机，节点与边的流转在 AgentFsm.c 里
 * @param context       上下文管理器
 * @param tools         工具定义 JSON 数组
 * @param on_reasoning  思考过程回调
 * @param on_tool_call  工具调用回调
 * @param on_context    回复内容回调
 */
static const AgentFsmOps g_fsm_ops = {
    .emit_answer = agent_emit_answer,
    .notify = agent_notify,
};

static void agent_loop(Context_t context,
                       cJSON *tools,
                       void (*on_reasoning)(const char *text),
                       void (*on_tool_call)(const char *text),
                       void (*on_context)(const char *text))
{
    AgentFsm_t fsm;

    rt_memset(&fsm, 0, sizeof(fsm));
    fsm.context = context;
    fsm.tools = tools;
    fsm.ops = &g_fsm_ops;
    fsm.on_reasoning = on_reasoning;
    fsm.on_tool_call = on_tool_call;
    fsm.on_context = on_context;
    fsm.loop_max = AGENT_LOOP_MAX;
    fsm.chat_retry_max = AGENT_CHAT_RETRY_MAX;

    agent_fsm_run(&fsm);
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

        g_turn_spontaneous = messages->spontaneous;

        /* 追加前先看占用：快满了就先压缩，给本轮输入腾地方。
           压缩失败不拦本轮，追加失败才跳过 */
        if (agent_context_arena_used(g_context) * 4 > agent_context_arena_capacity(g_context) * 3 &&
            g_context->compress_context(g_context, PKG_AGENT_MESSAGE_TRIM) != RT_EOK)
        {
            LOG_W("Context nearly full and compression failed; trying to append anyway.");
        }

        if (g_context->append_user_message(g_context, messages) != RT_EOK)
        {
            LOG_E("Context is full, cannot append the user message; skipping this turn");
            messages_destroy(messages);
            continue;
        }

        /* 输入消息所有权在 main_loop（消费者），使用完毕后释放 */
        messages_destroy(messages);

        g_agent_busy = RT_TRUE;
        agent_loop(g_context, get_agent_tools(), print_reasoning, print_tool_call, print_context);
        if (g_context->compress_context(g_context, PKG_AGENT_MESSAGE_TRIM) != RT_EOK)
        {
            LOG_W("Context compression failed; original history retained.");
        }
        g_agent_busy = RT_FALSE;
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

    /* 4. 销毁上下文，落盘记忆（开了持久化时） */
    agent_memory_deinit();
    if (g_context)
    {
        agent_context_destroy(g_context);
        g_context = RT_NULL;
    }

    /* 5. 清理工具系统（释放工具链表与残留的工具结果消息），再次进入时重新注册 */
    agent_tools_cleanup();

    agent_stream_deinit();
    agent_report_deinit();

    g_main_agent_loop = RT_NULL;
    g_agent_running = RT_FALSE;
    g_agent_busy = RT_FALSE;
    g_turn_spontaneous = RT_FALSE;
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