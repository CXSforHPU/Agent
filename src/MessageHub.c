#include "MessageHub.h"

/* 静态函数声明 */
static rt_err_t _put_message(MessageHub_t self, Message_t message,rt_mailbox_t mb);
static Message_t _get_message(MessageHub_t self,rt_mailbox_t mb);

/* 创建消息 */
Message_t message_create(MessageChannelType channel_type,char *content,rt_err_t is_free_content)
{
    Message_t message = (Message_t)rt_malloc(sizeof(Message));
    if (message == RT_NULL) {
        LOG_E("message malloc failed.");
        return RT_NULL;
    }

    message->channel_type = channel_type;
    message->content = rt_strdup(content);

    if (is_free_content)
    {
        rt_free(content);
    }
    

    return message;
}

/* 销毁消息 */
void message_destroy(Message_t message)
{
    if (message == RT_NULL) {
        return;
    }
    if (message->content != RT_NULL) {
        rt_free(message->content);
    }

    rt_free(message);
}

/* 发送消息 */
static rt_err_t _put_message(MessageHub_t self, Message_t message,rt_mailbox_t mb)
{
    if (self == RT_NULL || message == RT_NULL) {
        return -RT_ERROR;
    }

    return rt_mb_send(mb,(rt_ubase_t)message);
}

/* 接收消息 */
static Message_t _get_message(MessageHub_t self,rt_mailbox_t mb)
{
    Message_t message = RT_NULL;
    if (self == RT_NULL) {
        return RT_NULL;
    }
    rt_mb_recv(mb, (rt_ubase_t *) &message, RT_WAITING_FOREVER);

    return message;
}

/* 创建消息中心 */
MessageHub_t MessageHub_create(void)
{
    MessageHub_t hub = (MessageHub_t)rt_calloc(1, sizeof(MessageHub));
    if (hub == RT_NULL) {
        return RT_NULL;
    }

    hub->input_mailbox = rt_mb_create(MessageHub_INPUT_NAME,MessageHub_INPUT_MAILBOX_SIZE*sizeof(Message_t),RT_IPC_FLAG_FIFO);
    hub->output_mailbox = rt_mb_create(MessageHub_OUTPUT_NAME,MessageHub_INPUT_MAILBOX_SIZE*sizeof(Message_t),RT_IPC_FLAG_FIFO);
    /* 设置函数指针 */
    hub->put_message = _put_message;
    hub->get_message = _get_message;

    return hub;
}


/* 销毁消息中心 */
void MessageHub_destroy(MessageHub_t hub)
{
    if (hub == RT_NULL) {
        return;
    }

    if (hub->input_mailbox != RT_NULL) {
        rt_mb_delete(hub->input_mailbox);
    }
    if (hub->output_mailbox != RT_NULL) {
        rt_mb_delete(hub->output_mailbox);
    }

    rt_free(hub);
    return;
}

/* 公共接口：发送消息 */
rt_err_t MessageHub_put(MessageHub_t hub, Message_t message,rt_mailbox_t mb)
{
    if (hub == RT_NULL || hub->put_message == RT_NULL) {
        return -RT_ERROR;
    }
    return hub->put_message(hub, message,mb);
}

/* 公共接口：接收消息 */
Message_t MessageHub_get(MessageHub_t hub,rt_mailbox_t mb)
{
    if (hub == RT_NULL || hub->get_message == RT_NULL) {
        return RT_NULL;
    }

    return hub->get_message(hub,mb);
}