/*
 * MQTT 工具 - 接收线程与消息路由（门控放行、错峰、批量合并注入）
 * 接收线程周期同步订阅状态，并对每条消息执行过滤门控与投递决策。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"
#include "utils.h"          /* agent_inject_text() */

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 批量注入暂存：仅接收线程访问，无需加锁 */
static mqtt_rx_item_t s_batch[PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX];


/* ==========================================================================
 * 接收线程：订阅同步 + 消息分发
 * ========================================================================== */

/*
 * @brief 处理一条收到的消息：按投递模式决定是否交给 agent 分析
 * @note  filter 模式（默认）下只有「像咨询」或「命中阈值规则」的消息才会打扰 agent，
 *        其余只更新统计并保留在滚动窗口里，需要时用 mqtt_receive 按需取用
 * @param item 消息
 */
static void mqtt_route_item(const mqtt_rx_item_t *item)
{
    mqtt_sub_entry_t *entry;
    mqtt_deliver_mode_t mode = MQTT_DELIVER_FILTER;
    rt_bool_t forward = RT_FALSE;
    char reason[MQTT_TOOL_REASON_MAX_LEN];
    rt_size_t used = 0;
    char *text = RT_NULL;
    int count = 0;
    int dup_count = 0;

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
     * 错峰：agent 正在处理上一轮对话（LLM 请求 + 工具执行）时先等待其空闲，
     * 避免连续请求把 API/网络打爆——请求过密会触发对端重置连接，
     * 表现为 mbedtls NET_RECV_FAILED / chat POST failed。
     * 等待期间消息留在接收队列中，空闲后一次性合并注入。
     */
    if (agent_is_busy())
    {
        rt_int32_t waited = 0;

        LOG_I("agent busy, hold mqtt message (%s) until it is idle", item->topic);
        while (agent_is_busy() && waited < PKG_AGENT_TOOL_MQTT_BUSY_WAIT_MS)
        {
            rt_thread_mdelay(MQTT_TOOL_POLL_STEP_MS);
            waited += MQTT_TOOL_POLL_STEP_MS;
        }

        if (agent_is_busy())
        {
            /* 等待超时（对话异常长）：转 poll 缓冲，等 agent 空闲后可经 mqtt_receive 取回 */
            LOG_W("agent still busy after %d ms, buffer mqtt message from %s",
                  (int)PKG_AGENT_TOOL_MQTT_BUSY_WAIT_MS, item->topic);
            poll_push(item);
            return;
        }
    }

    /* 限速：避免高频话题把 LLM 调用打爆（消息仍留在队列中，不会丢） */
    {
        rt_tick_t interval = rt_tick_from_millisecond(PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS);
        rt_tick_t elapsed = rt_tick_get() - s_last_inject_tick;

        if (elapsed < interval)
        {
            rt_thread_mdelay(ticks_to_ms(interval - elapsed));
        }
    }

    text = rt_malloc(MQTT_TOOL_INJECT_TEXT_MAX);
    if (text == RT_NULL)
    {
        poll_push(item);
        return;
    }
    text[0] = '\0';

    s_batch[0] = *item;
    count = 1;
    buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used,
               PKG_AGENT_TOOL_MQTT_INJECT_FMT, item->topic, item->payload, reason);

    /* 合并队列中已就绪的若干条消息，减少 LLM 调用次数；未放行的消息转滚动窗口 */
    while (count < PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX)
    {
        mqtt_rx_item_t extra;
        rt_bool_t extra_forward = RT_FALSE;
        mqtt_deliver_mode_t extra_mode = MQTT_DELIVER_FILTER;

        if (rt_mq_recv(s_rx_queue, &extra, sizeof(extra), RT_WAITING_NO) <= 0)
        {
            break;
        }

        if (!lock_take())
        {
            /* 状态锁异常：消息不丢，转为缓存，避免在无锁状态下访问订阅表 */
            poll_push(&extra);
            continue;
        }
        entry = sub_match(extra.topic);
        if (entry != RT_NULL)
        {
            entry->rx_count++;
            entry->last_tick = rt_tick_get();
            rt_strncpy(entry->last_payload, extra.payload, PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN - 1);
            entry->last_payload[PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN - 1] = '\0';
            mqtt_hist_push(entry, &extra);
            extra_mode = entry->mode;
        }
        s_rx_total++;
        lock_give();

        /* 同批消息同样要过门控：只有 auto 模式、像咨询、或命中阈值规则的才并入本次注入 */
        if (extra_mode == MQTT_DELIVER_AUTO)
        {
            extra_forward = RT_TRUE;
        }
        else if (extra_mode == MQTT_DELIVER_FILTER)
        {
            extra_forward = payload_looks_like_ask(extra.payload)
                            ? RT_TRUE : rule_gate(&extra, RT_NULL, 0);
        }

        if (!extra_forward)
        {
            if (lock_take())
            {
                entry = sub_match(extra.topic);
                if (entry != RT_NULL)
                {
                    entry->suppressed_count++;
                }
                s_suppressed_total++;
                lock_give();
            }
            poll_push(&extra);
            continue;
        }

        /* 周期上报场景常出现完全相同的重复报文：同一批内折叠，只统计条数 */
        if (rt_strcmp(s_batch[count - 1].topic, extra.topic) == 0 &&
            rt_strcmp(s_batch[count - 1].payload, extra.payload) == 0)
        {
            dup_count++;
            continue;
        }

        s_batch[count] = extra;
        count++;
        buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used,
                   "\n---\ntopic: %s\npayload: %s", extra.topic, extra.payload);
    }

    if (dup_count > 0)
    {
        buf_append(text, MQTT_TOOL_INJECT_TEXT_MAX, &used,
                   "\n\nNote: %d additional message(s) with identical topic and payload "
                   "arrived in the same batch and are not repeated above.", dup_count);
    }

    /* 注入到 agent：公共 API，其它工具/驱动也用同一入口（见 include/utils.h） */
    if (agent_inject_text(text) == RT_EOK)
    {
        int i;
        LOG_I("mqtt -> agent: %d message(s) delivered for analysis", count);

        /* MQTT 侧自有的注入统计与错峰时间戳：注入成功后更新 */
        s_last_inject_tick = rt_tick_get();
        if (lock_take())
        {
            s_inject_total++;
            for (i = 0; i < count; i++)
            {
                entry = sub_match(s_batch[i].topic);
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
        int i;
        for (i = 0; i < count; i++)
        {
            poll_push(&s_batch[i]);
        }
    }

    rt_free(text);
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

        /* 2. 等待消息（短超时：保证订阅同步与退出响应及时） */
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

