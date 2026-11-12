/*
 * 流式事件队列。agent 干活时把思考、工具调用、正文分片放进来，
 * 需要实时看到过程的通道（如 A2A）边等结果边取走并推给客户端。
 * 队列满时覆盖最旧的一条，发送方永不阻塞。
 */
#include "AgentStream.h"

#define LOG_TAG "Agent.stream"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static struct rt_mutex g_lock;
static rt_bool_t g_ready = RT_FALSE;
static agent_stream_event_t g_queue[AGENT_STREAM_QUEUE_SIZE];
static rt_uint16_t g_head = 0;   /* 下一条写入位置 */
static rt_uint16_t g_count = 0;  /* 当前条数 */

void agent_stream_init(void)
{
    if (g_ready)
    {
        return;
    }
    if (rt_mutex_init(&g_lock, "agentStream", RT_IPC_FLAG_FIFO) == RT_EOK)
    {
        g_head = 0;
        g_count = 0;
        g_ready = RT_TRUE;
    }
    else
    {
        LOG_E("stream queue mutex init failed");
    }
}

void agent_stream_deinit(void)
{
    if (!g_ready)
    {
        return;
    }
    rt_mutex_detach(&g_lock);
    g_ready = RT_FALSE;
    g_head = 0;
    g_count = 0;
}

void agent_stream_emit(agent_stream_kind_t kind, const char *text)
{
    rt_size_t len;
    agent_stream_event_t *slot;

    if (!g_ready || text == RT_NULL || text[0] == '\0')
    {
        return;
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);

    slot = &g_queue[g_head];
    slot->kind = kind;
    len = rt_strlen(text);
    if (len >= AGENT_STREAM_TEXT_MAX)
    {
        len = AGENT_STREAM_TEXT_MAX - 4;
        rt_memcpy(slot->text, text, len);
        rt_memcpy(slot->text + len, "...", 4);
        len += 3;
    }
    else
    {
        rt_memcpy(slot->text, text, len + 1);
    }
    slot->len = len;

    g_head = (rt_uint16_t)((g_head + 1) % AGENT_STREAM_QUEUE_SIZE);
    if (g_count < AGENT_STREAM_QUEUE_SIZE)
    {
        g_count++;
    }
    else
    {
        /* 队列满：新事件写在了最旧那条的槽位上，最旧的一条被挤掉 */
        g_count = AGENT_STREAM_QUEUE_SIZE;
    }

    rt_mutex_release(&g_lock);
}

rt_bool_t agent_stream_take(agent_stream_event_t *event)
{
    rt_bool_t got = RT_FALSE;

    if (!g_ready || event == RT_NULL)
    {
        return RT_FALSE;
    }

    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    if (g_count > 0)
    {
        rt_uint16_t tail = (rt_uint16_t)((g_head + AGENT_STREAM_QUEUE_SIZE - g_count)
                                         % AGENT_STREAM_QUEUE_SIZE);

        rt_memcpy(event, &g_queue[tail], sizeof(agent_stream_event_t));
        g_count--;
        got = RT_TRUE;
    }
    rt_mutex_release(&g_lock);

    return got;
}

void agent_stream_clear(void)
{
    if (!g_ready)
    {
        return;
    }
    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    g_head = 0;
    g_count = 0;
    rt_mutex_release(&g_lock);
}

const char *agent_stream_kind_name(agent_stream_kind_t kind)
{
    switch (kind)
    {
    case AGENT_STREAM_REASONING: return "reasoning";
    case AGENT_STREAM_TOOL_CALL: return "tool";
    case AGENT_STREAM_CONTENT:   return "content";
    default:                     return "notice";
    }
}
