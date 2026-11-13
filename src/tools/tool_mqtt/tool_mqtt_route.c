/*
 * MQTT 工具 - 接收线程与消息路由：门控放行、攒批、合并注入
 *
 * 接收线程只做两件事：把收到的消息过一遍门控，把放行的消息攒进待注入队列。
 * 它不阻塞等待 agent 空闲：等待期间原始队列会堆满，例行数据被整片丢掉。
 */
#include "tool_mqtt_internal.h"
#include "utils.h"          /* agent_inject_text() */

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 待注入队列：agent 忙时先攒着，空闲后合并成一次注入。仅接收线程访问，无需加锁 */
static mqtt_pending_t s_pending[MQTT_TOOL_PENDING_DEPTH];
static int s_pending_count = 0;

/* ==========================================================================
 * 待注入队列
 * ========================================================================== */

/*
 * @brief 当前攒着几条待注入的消息
 */
int mqtt_pending_count(void)
{
    return s_pending_count;
}

/*
 * @brief 把一条放行的消息攒进待注入队列
 * @param item   消息
 * @param reason 放行原因
 * @note  攒不下时丢最旧的一条：新的告警比旧的值钱
 */
static void pending_push(const mqtt_rx_item_t *item, const char *reason)
{
    int i;

    if (s_pending_count >= MQTT_TOOL_PENDING_DEPTH)
    {
        for (i = 1; i < MQTT_TOOL_PENDING_DEPTH; i++)
        {
            s_pending[i - 1] = s_pending[i];
        }
        s_pending_count--;
        s_rx_dropped++;
        LOG_W("mqtt pending queue full, drop the oldest alert on topic %s",
              s_pending[0].item.topic);
    }

    s_pending[s_pending_count].item = *item;
    rt_strncpy(s_pending[s_pending_count].reason, (reason != RT_NULL) ? reason : "",
               MQTT_TOOL_REASON_MAX_LEN - 1);
    s_pending[s_pending_count].reason[MQTT_TOOL_REASON_MAX_LEN - 1] = '\0';
    s_pending_count++;
}

/*
 * @brief 两次注入之间的最小间隔是否已过
 * @note  限速保护 LLM 端点：请求发太密会被对端重置连接
 */
static rt_bool_t inject_interval_ok(void)
{
    rt_tick_t interval = rt_tick_from_millisecond(PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS);
    rt_tick_t elapsed = rt_tick_get() - s_last_inject_tick;

    return (elapsed >= interval) ? RT_TRUE : RT_FALSE;
}


/* ==========================================================================
 * 接收线程：订阅同步 + 消息分发
 * ========================================================================== */

/*
 * @brief 把攒着的消息合并成一次注入
 * @note  一次最多带 PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX 条，
 *        完全相同的重复报文只记条数，剩下的留在队列里下次再注入
 */
static void deliver_pending(void)
{
    char *text;
    rt_size_t used = 0;
    int kept = 0;
    int dup_count = 0;
    int take;
    int i;
    int k;

    if (s_pending_count == 0)
    {
        return;
    }

    /* 折叠完全相同的重复报文：周期上报常见 */
    for (i = 0; i < s_pending_count; i++)
    {
        if (kept > 0 &&
            rt_strcmp(s_pending[kept - 1].item.topic, s_pending[i].item.topic) == 0 &&
            rt_strcmp(s_pending[kept - 1].item.payload, s_pending[i].item.payload) == 0)
        {
            dup_count++;
            continue;
        }
        if (kept != i)
        {
            s_pending[kept] = s_pending[i];
        }
        kept++;
    }
    s_pending_count = kept;

    take = (kept < PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX) ? kept
                                                        : PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX;

    text = rt_malloc(MQTT_TOOL_INJECT_TEXT_MAX);
    if (text == RT_NULL)
    {
        return;
    }
    text[0] = '\0';

    for (i = 0; i < take; i++)
    {
        if (i == 0)
        {
            buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used, PKG_AGENT_TOOL_MQTT_INJECT_FMT,
                       s_pending[i].item.topic, s_pending[i].item.payload, s_pending[i].reason);
        }
        else
        {
            buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used,
                       "\n---\ntopic: %s\npayload: %s\nforward reason: %s",
                       s_pending[i].item.topic, s_pending[i].item.payload, s_pending[i].reason);
        }
    }

    if (dup_count > 0)
    {
        buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used,
                   "\n\nNote: %d additional message(s) with identical topic and payload "
                   "arrived in the same batch and are not repeated above.", dup_count);
    }

    if (agent_inject_text(text) == RT_EOK)
    {
        mqtt_sub_entry_t *entry;

        LOG_I("mqtt -> agent: %d message(s) delivered for analysis", take);

        /* MQTT 侧自有的注入统计与错峰时间戳：注入成功后更新 */
        s_last_inject_tick = rt_tick_get();
        if (lock_take())
        {
            s_inject_total++;
            for (k = 0; k < take; k++)
            {
                entry = sub_match(s_pending[k].item.topic);
                if (entry != RT_NULL)
                {
                    entry->inject_count++;
                }
            }
            lock_give();
        }
    }
    else
    {
        /* 注入失败（agent 停止/mailbox 满）：转存轮询缓冲，避免消息丢失 */
        for (k = 0; k < take; k++)
        {
            poll_push(&s_pending[k].item);
        }
    }

    /* 本次没带走的留在队列里，下次注入时一起给 agent */
    for (i = take; i < kept; i++)
    {
        s_pending[i - take] = s_pending[i];
    }
    s_pending_count = kept - take;

    rt_free(text);
}

/*
 * @brief 处理一条收到的消息：按投递模式决定是否交给 agent 分析
 * @param item 消息
 * @note  filter 模式（默认）下只有「像咨询」或「命中阈值规则」的消息才会打扰 agent，
 *        其余只更新统计并保留在滚动窗口里，需要时用 mqtt_receive 按需取用
 */
static void mqtt_route_item(const mqtt_rx_item_t *item)
{
    mqtt_sub_entry_t *entry;
    mqtt_deliver_mode_t mode = MQTT_DELIVER_FILTER;
    rt_bool_t forward = RT_FALSE;
    char reason[MQTT_TOOL_REASON_MAX_LEN];

    if (item == RT_NULL)
    {
        return;
    }

    reason[0] = '\0';

    if (!lock_take())
    {
        LOG_W("mqtt state lock busy, drop message from %s", item->topic);
        s_rx_dropped++;
        return;
    }
    entry = sub_match(item->topic);
    if (entry != RT_NULL)
    {
        entry->rx_count++;
        entry->last_tick = rt_tick_get();
        rt_strncpy(entry->last_payload, item->payload, PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN - 1);
        entry->last_payload[PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN - 1] = '\0';
        mqtt_hist_push(entry, item);   /* 例行数据同样进近期历史，供 mqtt_history 总结 */
        mode = entry->mode;
    }
    s_rx_total++;
    lock_give();

    /* 未被任何订阅匹配：按 filter 处理（只有咨询/命中规则才放行） */
    if (mode == MQTT_DELIVER_POLL)
    {
        poll_push(item);
        return;
    }

    if (mode == MQTT_DELIVER_AUTO)
    {
        forward = RT_TRUE;
        rt_strncpy(reason, "topic is in auto mode (forward every message)", sizeof(reason) - 1);
    }
    else
    {
        /* filter：先看是否像咨询，再看是否命中阈值规则 */
        if (payload_looks_like_ask(item->payload))
        {
            forward = RT_TRUE;
            rt_strncpy(reason, "message looks like a request for advice or an alert", sizeof(reason) - 1);
        }
        else if (rule_gate(item, reason, sizeof(reason)))
        {
            forward = RT_TRUE;
        }
    }

    if (!forward)
    {
        /* 本地拦截：不调用 LLM，仅统计 + 进滚动窗口，供按需总结 */
        if (lock_take())
        {
            entry = sub_match(item->topic);
            if (entry != RT_NULL)
            {
                entry->suppressed_count++;
            }
            s_suppressed_total++;
            lock_give();
        }
        poll_push(item);
        return;
    }

    /* agent 未运行：先缓存，agent 启动后可经 mqtt_receive 取回 */
    if (!agent_is_running() || agent_get_message_hub() == RT_NULL)
    {
        LOG_I("agent not running, buffer mqtt message from %s", item->topic);
        poll_push(item);
        return;
    }

    /*
     * 放行的消息先攒起来：接收线程在这里继续收下一条，
     * 不等 agent 空闲（等的话原始队列会堆满，例行数据被整片丢掉）。
     * agent 空闲后由接收线程合并注入，避免连续请求把 LLM 端点打爆。
     */
    pending_push(item, reason);
}

/*
 * @brief 接收线程主体：周期同步订阅状态并分发收到的消息
 * @param param 未使用
 */
void mqtt_rx_thread(void *param)
{
    mqtt_rx_item_t item;

    RT_UNUSED(param);

    while (s_rx_running)
    {
        /* 1. 订阅/退订落地（含连接后补订阅）；同步函数内部自行加解锁 */
        subs_sync_with_client();

        /* 2. agent 空闲且攒着消息：合并注入一次 */
        if (s_pending_count > 0 && !agent_is_busy() && inject_interval_ok())
        {
            deliver_pending();
            continue;
        }

        /* 3. 等待消息（短超时：保证订阅同步与退出响应及时） */
        if (rt_mq_recv(s_rx_queue, &item, sizeof(item), rt_tick_from_millisecond(MQTT_TOOL_RX_WAIT_MS)) <= 0)
        {
            continue;
        }

        mqtt_route_item(&item);
    }

    LOG_I("mqtt rx thread exit");
    if (s_rx_exit_sem != RT_NULL)
    {
        rt_sem_release(s_rx_exit_sem);
    }
}

