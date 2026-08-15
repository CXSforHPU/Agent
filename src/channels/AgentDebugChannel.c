#include "AgentDebugChannel.h"
#include <stdlib.h>   // for atoi

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;   // 当前未使用，保留

int AgentDebugChannel(MessageHub_t message_hub, Context_t context)
{
    if (g_message_hub == RT_NULL) {
        g_message_hub = message_hub;
    }
    if (g_context == RT_NULL) {
        g_context = context;
    }
    return 0;   // 补充返回值
}

/*
typedef enum MessageType
{
    TYPE_TEXT,
    TYPE_AUDIO,
    TYPE_IMAGE,
    TYPE_VIDEO,
    TYPE_NULL
} MessageType;
*/

static int agent_send_message(int argc, char *argv[])   // 修正参数顺序及类型
{
    // 1. 参数个数校验
    if (argc < 3) {
        return -1;
    }

    // 2. 检查全局句柄是否已初始化
    if (g_message_hub == RT_NULL) {
        return -1;
    }

    // 3. 解析消息类型（假设 atoi 能正确处理，实际可增加范围检查）
    int type_int = atoi(argv[1]);
    if (type_int < TYPE_TEXT || type_int > TYPE_NULL) {
        return -1;
    }
    MessageType msg_type = (MessageType)type_int;

    char *content = argv[2];

    // 4. 创建输入消息
    Messages_t input_messages = message_create(msg_type, content, 1);
    if (input_messages == RT_NULL) {
        return -1;
    }

    // 5. 将输入消息放入输入邮箱（假设 put_message 原型为 int put_message(MessageHub_t, void*, Message_t)）
    rt_err_t ret = g_message_hub->put_message(g_message_hub, input_messages, g_message_hub->input_mailbox);
    if (ret == RT_ERROR) {
        messages_destroy(input_messages);
        return -1;
    }

    // 6. 从输出邮箱获取响应消息（若暂无消息，可能阻塞或立即返回 NULL，取决于具体实现）
    //    此处简单处理：若返回 NULL，视为失败，但输入消息已发出，需根据业务决定是否继续等待。
    //    为保持简单，若输出为 NULL 仍销毁输入并返回 -1，但也可选择等待机制。
    Messages_t output_message = g_message_hub->get_message(g_message_hub, g_message_hub->output_mailbox);

    // 7. 销毁输入消息（无论是否获得输出）
    message_destroy(input_message);

    // 8. 处理输出消息
    if (output_message == RT_NULL) {
        // 没有收到响应，返回错误（实际可根据需要重试或忽略）
        return -1;
    }

    // 成功获取输出，可在此处理 output_message 内容，例如打印或转发
    // 处理完毕后销毁
    message_destroy(output_message);

    return 0;
}

// static int agent_send_message()   // 修正参数顺序及类型
// {

//     Message_t messages[2];

//     messages[0] = message_create(TYPE_IMAGE,"/test/test.jpg",2);
//     messages[1] = message_create(TYPE_TEXT,"intorduce the picture",2);
//     // 5. 将输入消息放入输入邮箱（假设 put_message 原型为 int put_message(MessageHub_t, void*, Message_t)）

//     rt_err_t ret = g_message_hub->put_message(g_message_hub, messages[0], g_message_hub->input_mailbox);
//     if (ret == RT_ERROR) {
//         message_destroy(messages[0]);
//         message_destroy(messages[1]);
//         return -1;
//     }

//     // 6. 从输出邮箱获取响应消息（若暂无消息，可能阻塞或立即返回 NULL，取决于具体实现）
//     //    此处简单处理：若返回 NULL，视为失败，但输入消息已发出，需根据业务决定是否继续等待。
//     //    为保持简单，若输出为 NULL 仍销毁输入并返回 -1，但也可选择等待机制。
//     Message_t output_message = g_message_hub->get_message(g_message_hub, g_message_hub->output_mailbox);

//     message_destroy(messages[0]);
//     message_destroy(messages[1]);

//     // 8. 处理输出消息
//     if (output_message == RT_NULL) {
//         // 没有收到响应，返回错误（实际可根据需要重试或忽略）
//         return -1;
//     }

//     // 成功获取输出，可在此处理 output_message 内容，例如打印或转发
//     // 处理完毕后销毁
//     message_destroy(output_message);

//     return 0;

// }

// 导出命令，第二个参数为帮助描述字符串（用引号括起）
MSH_CMD_EXPORT(agent_send_message, "agent_send_message <type> <content>");