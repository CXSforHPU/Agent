#include "MessageHub.h"

static char agent_content_type[4][16] = {"text", "audio_url", "image_url", "video_url"};

/* 静态函数声明 */
static rt_err_t _put_message(MessageHub_t self, Messages_t message, rt_mailbox_t mb);
static Messages_t _get_message(MessageHub_t self, rt_mailbox_t mb);

/*
 * @brief 获取消息类型对应的字符串
 * @param message_type 消息类型枚举
 * @return 类型字符串指针
 */
char *get_agent_content_type(MessageType message_type)
{
    return agent_content_type[message_type];
}

/*
 * @brief 创建消息数组
 * @param max_size 初始容量
 * @return Messages_t 成功返回句柄，失败返回 NULL
 */
Messages_t messages_create(int max_size)
{
    Messages_t messages = (Messages_t)rt_malloc(sizeof(Messages));
    if (messages == RT_NULL)
    {
        LOG_E("malloc messages failed");
        return RT_NULL;
    }
    if (max_size == 0)
    {
        messages->message_ptr = RT_NULL;
    }
    else
    {
        messages->message_ptr = (MessageItem_t)rt_malloc(max_size * sizeof(MessageItem));
    }

    if (messages->message_ptr == RT_NULL && max_size != 0)
    {
        LOG_E("malloc message_ptr failed");
        rt_free(messages);
        return RT_NULL;
    }

    messages->max_size = max_size;
    messages->current_size = 0;
    return messages;
}

/*
 * @brief 向消息数组追加一条消息
 * @param messages     消息数组句柄
 * @param message_type 消息类型
 * @param content      消息内容
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t messages_append(Messages_t messages, MessageType message_type, const char *content)
{
    if (messages == RT_NULL || content == RT_NULL)
    {
        return RT_ERROR;
    }

    int idx = messages->current_size;

    /* 需要扩容 */
    if (idx >= messages->max_size)
    {
        int new_max = messages->max_size + 5;
        MessageItem_t new_ptr = (MessageItem_t)rt_realloc(messages->message_ptr, new_max * sizeof(MessageItem));
        if (new_ptr == RT_NULL)
        {
            LOG_E("realloc failed");
            return RT_ERROR;
        }
        messages->message_ptr = new_ptr;
        messages->max_size = new_max;
    }

    char *dup_content = rt_strdup(content);
    if (dup_content == RT_NULL)
    {
        LOG_E("rt_strdup failed");
        return RT_ERROR;
    }

    messages->message_ptr[idx].content = dup_content;
    messages->message_ptr[idx].message_type = message_type;
    messages->current_size++;

    return RT_EOK;
}

/*
 * @brief 销毁消息数组并释放所有内容
 * @param messages 消息数组句柄
 */
void messages_destroy(Messages_t messages)
{
    if (messages == RT_NULL)
    {
        return;
    }

    if (messages->message_ptr)
    {
        for (int i = 0; i < messages->current_size; i++)
        {
            if (messages->message_ptr[i].content)
            {
                rt_free(messages->message_ptr[i].content);
            }
        }
        rt_free(messages->message_ptr);
    }
    rt_free(messages);
}

/*
 * @brief 获取指定索引的消息内容
 * @param messages 消息数组句柄
 * @param idx      索引
 * @return 内容字符串指针
 */
char *messages_get_content_idx(Messages_t messages, int idx)
{
    if (idx >= messages->current_size)
    {
        LOG_E("idx is long of current size");
        return RT_NULL;
    }

    return messages->message_ptr[idx].content;
}

/*
 * @brief 获取指定索引的消息类型
 * @param messages 消息数组句柄
 * @param idx      索引
 * @return 消息类型枚举
 */
MessageType messages_get_type_idx(Messages_t messages, int idx)
{
    if (idx >= messages->current_size)
    {
        LOG_E("idx is long of current size");
        return TYPE_NULL;
    }

    return messages->message_ptr[idx].message_type;
}

/* 发送消息到 mailbox */
static rt_err_t _put_message(MessageHub_t self, Messages_t message, rt_mailbox_t mb)
{
    if (self == RT_NULL || message == RT_NULL)
    {
        return -RT_ERROR;
    }

    return rt_mb_send(mb, (rt_ubase_t)message);
}

/* 从 mailbox 接收消息 */
static Messages_t _get_message(MessageHub_t self, rt_mailbox_t mb)
{
    Messages_t messages = RT_NULL;
    if (self == RT_NULL)
    {
        return RT_NULL;
    }
    rt_mb_recv(mb, (rt_ubase_t *)&messages, RT_WAITING_FOREVER);

    return messages;
}

/*
 * @brief 创建消息中心
 * @return MessageHub_t 成功返回句柄，失败返回 NULL
 */
MessageHub_t message_hub_create(void)
{
    MessageHub_t hub = (MessageHub_t)rt_calloc(1, sizeof(MessageHub));
    if (hub == RT_NULL)
    {
        return RT_NULL;
    }

    hub->input_mailbox = rt_mb_create(MESSAGE_HUB_INPUT_NAME, MESSAGE_HUB_INPUT_MAILBOX_SIZE, RT_IPC_FLAG_FIFO);
    hub->output_mailbox = rt_mb_create(MESSAGE_HUB_OUTPUT_NAME, MESSAGE_HUB_OUTPUT_MAILBOX_SIZE, RT_IPC_FLAG_FIFO);
    hub->put_message = _put_message;
    hub->get_message = _get_message;

    return hub;
}

/*
 * @brief 销毁消息中心
 * @param hub 消息中心句柄
 */
void message_hub_destroy(MessageHub_t hub)
{
    if (hub == RT_NULL)
    {
        return;
    }

    if (hub->input_mailbox != RT_NULL)
    {
        rt_mb_delete(hub->input_mailbox);
    }
    if (hub->output_mailbox != RT_NULL)
    {
        rt_mb_delete(hub->output_mailbox);
    }

    rt_free(hub);
}

/*
 * @brief 发送消息（公共接口）
 * @param hub     消息中心句柄
 * @param message 消息数组
 * @param mb      目标 mailbox
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t message_hub_put(MessageHub_t hub, Messages_t message, rt_mailbox_t mb)
{
    if (hub == RT_NULL || hub->put_message == RT_NULL)
    {
        return -RT_ERROR;
    }
    return hub->put_message(hub, message, mb);
}

/*
 * @brief 接收消息（公共接口）
 * @param hub 消息中心句柄
 * @param mb  源 mailbox
 * @return 消息数组指针
 */
Messages_t message_hub_get(MessageHub_t hub, rt_mailbox_t mb)
{
    if (hub == RT_NULL || hub->get_message == RT_NULL)
    {
        return RT_NULL;
    }

    return hub->get_message(hub, mb);
}