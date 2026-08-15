#include "AgentDebugChannel.h"
#include <stdlib.h>

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;

/*
 * @brief Debug 通道初始化
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return 0 成功
 */
int agent_debug_channel(MessageHub_t message_hub, Context_t context)
{
    if (g_message_hub == RT_NULL) {
        g_message_hub = message_hub;
    }
    if (g_context == RT_NULL) {
        g_context = context;
    }
    return 0;
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
    messages_append(input_messages, msg_type, content);

    rt_err_t ret = g_message_hub->put_message(g_message_hub, input_messages, g_message_hub->input_mailbox);
    if (ret == RT_ERROR) {
        messages_destroy(input_messages);
        return -1;
    }

    Messages_t output_message = g_message_hub->get_message(g_message_hub, g_message_hub->output_mailbox);

    messages_destroy(input_messages);

    if (output_message == RT_NULL) {
        return -1;
    }

    messages_destroy(output_message);

    return 0;
}

MSH_CMD_EXPORT(agent_send_message, "agent_send_message <type> <content>");