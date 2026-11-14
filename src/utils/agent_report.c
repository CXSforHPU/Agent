#include "utils/agent_report.h"
static char s_queue[AGENT_REPORT_QUEUE_SIZE][AGENT_REPORT_TEXT_MAX];
static rt_uint16_t s_head, s_tail, s_count;
static struct rt_mutex s_lock;
static rt_bool_t s_inited;
rt_err_t agent_report_push(const char *text)
{
    rt_err_t rc = RT_EOK;

    if (text == RT_NULL || text[0] == '\0')
    {
        return RT_ERROR;
    }
    if (!s_inited)
    {
        return RT_ERROR;
    }

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_count >= AGENT_REPORT_QUEUE_SIZE)
    {
        /* 队列满了丢最旧的一条，位置不动 */
        s_head = (rt_uint16_t)((s_head + 1) % AGENT_REPORT_QUEUE_SIZE);
        s_count--;

    }
    rt_snprintf(s_queue[s_tail], AGENT_REPORT_TEXT_MAX, "%s", text);
    s_tail = (rt_uint16_t)((s_tail + 1) % AGENT_REPORT_QUEUE_SIZE);
    s_count++;
    rt_mutex_release(&s_lock);

    return rc;
}

/*
 * @brief 取出一条待上报的结论文本
 */
rt_err_t agent_report_take(char *out, rt_size_t size)
{
    rt_err_t rc = RT_ERROR;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_count > 0)
    {
        rt_snprintf(out, size, "%s", s_queue[s_head]);
        s_head = (rt_uint16_t)((s_head + 1) % AGENT_REPORT_QUEUE_SIZE);
        s_count--;
        rc = RT_EOK;
    }
    rt_mutex_release(&s_lock);

    return rc;
}


void agent_report_init(void)
{
    if (s_inited) return;
    if (rt_mutex_init(&s_lock, "report", RT_IPC_FLAG_FIFO) != RT_EOK) return;
    s_head = s_tail = s_count = 0; s_inited = RT_TRUE;
}
void agent_report_deinit(void)
{
    if (!s_inited) return;
    s_inited = RT_FALSE; rt_mutex_detach(&s_lock);
}
rt_bool_t agent_report_ready(void)
{
#ifdef PKG_AGENT_A2A_CHANNEL
    return s_inited;
#else
    return RT_FALSE;
#endif
}
