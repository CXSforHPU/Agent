#ifndef __AGENT_MESSAGE_HUB_H__
#define __AGENT_MESSAGE_HUB_H__

#include <rtthread.h>
#include "cJSON.h"

#define LOG_TAG "Agent.MessageHub"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define MESSAGE_HUB_INPUT_MAILBOX_SIZE 10
#define MESSAGE_HUB_OUTPUT_MAILBOX_SIZE 10
#define MESSAGE_HUB_INPUT_NAME "AgentInputMb"
#define MESSAGE_HUB_OUTPUT_NAME "AgentOutMb"

/* 消息通道类型 */
typedef enum MessageType
{
    TYPE_TEXT,
    TYPE_AUDIO,
    TYPE_IMAGE,
    TYPE_VIDEO,
    TYPE_NULL
} MessageType;

/* 消息结构 */
typedef struct MessageItem
{
    MessageType message_type;
    char *content;
} MessageItem, *MessageItem_t;

/* 消息数组结构 */
typedef struct Messages
{
    MessageItem_t message_ptr;
    int max_size;
    int current_size;
} Messages, *Messages_t;

/* 消息中心结构 */
typedef struct MessageHub
{
    /* 属性 */
    rt_mailbox_t input_mailbox;
    rt_mailbox_t output_mailbox;

    /* 方法 */
    rt_err_t (*put_message)(struct MessageHub *self, Messages_t message, rt_mailbox_t mb);
    Messages_t (*get_message)(struct MessageHub *self, rt_mailbox_t mb);
} MessageHub, *MessageHub_t;

/* 函数声明 */
MessageHub_t message_hub_create(void);
void message_hub_destroy(MessageHub_t hub);
rt_err_t message_hub_put(MessageHub_t hub, Messages_t message, rt_mailbox_t mb);
Messages_t message_hub_get(MessageHub_t hub, rt_mailbox_t mb);

/* 创建消息 */
Messages_t messages_create(int max_size);
rt_err_t messages_append(Messages_t messages, MessageType message_type, const char *content);
void messages_destroy(Messages_t messages);
MessageType messages_get_type_idx(Messages_t messages, int idx);
char *messages_get_content_idx(Messages_t messages, int idx);

/* 获取消息类型字符串 */
char *get_agent_content_type(MessageType message_type);

#endif /* __AGENT_MESSAGE_HUB_H__ */