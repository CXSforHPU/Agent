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
}MessageItem, *MessageItem_t;

typedef struct Messages
{
    MessageItem_t message_ptr;
    int max_size;
    int current_size;
}Messages,*Messages_t;



/* 消息中心结构 */
typedef struct MessageHub
{
    /* 属性 */
    rt_mailbox_t input_mailbox;
    rt_mailbox_t output_mailbox;

    /* 方法 */
    rt_err_t (*put_message)(struct MessageHub *self, Messages_t message,rt_mailbox_t mb);
    Messages_t (*get_message)(struct MessageHub *self,rt_mailbox_t mb);
} MessageHub, *MessageHub_t;

/* 函数声明 */
MessageHub_t MessageHub_create(void);
void MessageHub_destroy(MessageHub_t hub);
rt_err_t MessageHub_put(MessageHub_t hub, Messages_t message,rt_mailbox_t mb);
Messages_t MessageHub_get(MessageHub_t hub,rt_mailbox_t mb);

/* 创建消息 */
Messages_t messages_create(int max_size);
rt_err_t messages_append(Messages_t messages,MessageType message_type,const char* content);
void messages_destroy(Messages_t messages);
MessageType messages_get_type_idx(Messages_t messages,int idx);
char* messages_get_content_idx(Messages_t messages,int idx);
/* 获取消息类型 */
char* get_agent_content_type(MessageType message_type);

#endif /* __AGENT_MessageHub_H__ */