#ifndef __AGENT_MessageHub_H__
#define __AGENT_MessageHub_H__

#include <rtthread.h>
#include "cJSON.h"

#define LOG_TAG "Agent.MessageHub"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define MessageHub_INPUT_MAILBOX_SIZE 10
#define MessageHub_INPUT_NAME "AgentInputMb"
#define MessageHub_OUTPUT_NAME "AgentOutMb"

/* 消息通道类型 */
typedef enum MessageChannelType
{
    CHANNEL_CLI,
    CHANNEL_WEBNET,
    ASSITANT
} MessageChannelType;

/* 消息结构 */
typedef struct Message
{
    MessageChannelType channel_type;
    char *content;
} Message, *Message_t;

/* 消息中心结构 */
typedef struct MessageHub
{
    /* 属性 */
    rt_mailbox_t input_mailbox;
    rt_mailbox_t output_mailbox;

    /* 方法 */
    rt_err_t (*put_message)(struct MessageHub *self, Message_t message,rt_mailbox_t mb);
    Message_t (*get_message)(struct MessageHub *self,rt_mailbox_t mb);
} MessageHub, *MessageHub_t;

/* 函数声明 */
MessageHub_t MessageHub_create(void);
void MessageHub_destroy(MessageHub_t hub);
rt_err_t MessageHub_put(MessageHub_t hub, Message_t message,rt_mailbox_t mb);
Message_t MessageHub_get(MessageHub_t hub,rt_mailbox_t mb);

/* 创建消息 */
Message_t message_create(MessageChannelType channel_type,char *content,rt_err_t is_free_content);
void message_destroy(Message_t message);


#endif /* __AGENT_MessageHub_H__ */