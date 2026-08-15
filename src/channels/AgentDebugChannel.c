#include "AgentDebugChannel.h"
#include <stdlib.h>

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;

/* Debug 通道 ops 实例（init=agent_debug_channel, reset=agent_debug_reset） */
AgentChannelOps agent_debug_ops = { agent_debug_channel, agent_debug_reset };

/*
 * @brief Debug 通道初始化（无条件刷新缓存的 hub/context）
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return 0 成功
 */
int agent_debug_channel(MessageHub_t message_hub, Context_t context)
{
    /* 无条件刷新：agent 清理后 hub/context 可能已重建，
       避免旧指针残留导致下次使用时访问已释放内存 */
    g_message_hub = message_hub;
    g_context = context;
    return 0;
}

/*
 * @brief Debug 通道清理（置空缓存的 hub/context 指针，幂等）
 * @note 作为 AgentChannelOps.reset 实现，用于清理流程
 */
void agent_debug_reset(void)
{
    g_message_hub = RT_NULL;
    g_context = RT_NULL;
}

/*
 * @brief 发送消息到 agent（MSH 命令）
 * @param argc 参数个数
 * @param argv 参数列表：argv[1]=type, argv[2]=content
 * @return 0 成功，-1 失败
 */
static int agent_send_message(int argc, char *argv[])
{
    if (argc < 3) {
        return -1;
    }

    if (g_message_hub == RT_NULL) {
        return -1;
    }

    int type_int = atoi(argv[1]);
    if (type_int < TYPE_TEXT || type_int > TYPE_NULL) {
        return -1;
    }
    MessageType msg_type = (MessageType)type_int;

    char *content = argv[2];

    Messages_t input_messages = messages_create(1);
    if (input_messages == RT_NULL) {
        return -1;
    }
    if (messages_append(input_messages, msg_type, content) != RT_EOK) {
        messages_destroy(input_messages);
        return -1;
    }

    /* 所有权移交：put 成功后由 main_loop 消费并释放，此处不得销毁 */
    rt_err_t ret = g_message_hub->put_message(g_message_hub, input_messages, g_message_hub->input_mailbox);
    if (ret != RT_EOK) {
        messages_destroy(input_messages);
        return -1;
    }

    /* 可中断的输出等待：超时返回 NULL（如 agent 已停止/对话失败无输出） */
    Messages_t output_message = message_hub_get_timeout(g_message_hub,
                                                        g_message_hub->output_mailbox,
                                                        rt_tick_from_millisecond(30000));

    if (output_message == RT_NULL) {
        return -1;
    }

    messages_destroy(output_message);

    return 0;
}

MSH_CMD_EXPORT(agent_send_message, "agent_send_message <type> <content>");