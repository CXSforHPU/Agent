/*
 * MQTT 工具 - 业务操作（发布/订阅/退订/列表/收取），工具与 MSH 共用
 * 纯业务动作，不含 LLM 工具封装与命令行解析，便于被两侧复用与测试。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* ==========================================================================
 * 业务操作（工具函数与 MSH 命令共用）
 * ========================================================================== */

/*
 * @brief 计算当前条件下单次发布允许的最大 payload 长度
 * @param topic 目标话题
 * @return 允许的最大字节数
 */
static rt_size_t mqtt_publish_limit(const char *topic)
{
    rt_size_t overhead = (rt_size_t)sizeof(MQTTMessage) + rt_strlen(topic) + 1;
    rt_size_t limit = (rt_size_t)PKG_AGENT_TOOL_MQTT_MAX_PUB_LEN;
    rt_size_t by_buf;
    rt_size_t by_pipe;

    by_buf = (s_client.buf_size > overhead) ? (s_client.buf_size - overhead) : 0;
    by_pipe = (PKG_AGENT_TOOL_MQTT_PIPE_BUF_SIZE > overhead) ?
              ((rt_size_t)PKG_AGENT_TOOL_MQTT_PIPE_BUF_SIZE - overhead) : 0;

    if (by_buf < limit)
    {
        limit = by_buf;
    }
    if (by_pipe < limit)
    {
        limit = by_pipe;
    }
    return limit;
}

/*
 * @brief 连接 broker：未启动则启动，未连上则等待（发布/订阅前统一使用）
 * @param timeout_ms 等待超时（毫秒），<=0 用默认值
 * @param out        结果文本
 * @param out_size   结果缓冲大小
 * @return RT_EOK 已连接，RT_ERROR 失败
 */
rt_err_t mqtt_do_connect(rt_int32_t timeout_ms, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    rt_bool_t was_started = mqtt_tool_is_started();

    if (timeout_ms > 0)
    {
        /* 显式指定超时时：自己轮询，避免改动全局等待参数 */
        rt_int32_t waited = 0;

        if (!was_started && mqtt_tool_start() != RT_EOK)
        {
            buf_append(out, out_size, &used, "error: mqtt client start failed");
            return RT_ERROR;
        }
        buf_append(out, out_size, &used, "connect: broker=%s client_id=%s\n",
                   MQTT_SERVER_URI, MQTT_CLIENTID);
        while (waited < timeout_ms && !s_client.isconnected)
        {
            rt_thread_mdelay(MQTT_TOOL_POLL_STEP_MS);
            waited += MQTT_TOOL_POLL_STEP_MS;
        }
        if (s_client.isconnected)
        {
            buf_append(out, out_size, &used, "connect ok: state=%s waited=%d ms",
                       mqtt_tool_conn_state(), (int)waited);
            return RT_EOK;
        }
        buf_append(out, out_size, &used,
                   "error: connect timeout after %d ms (state=%s); the client keeps reconnecting "
                   "in the background - check network/broker address/credentials",
                   (int)timeout_ms, mqtt_tool_conn_state());
        return RT_ERROR;
    }

    if (mqtt_tool_ensure_connected(out, out_size))
    {
        return RT_EOK;
    }
    return RT_ERROR;
}

/*
 * @brief 断开连接并释放工作线程/接收线程/队列
 * @note  订阅意图（want）保留，下次连接时会自动重新订阅
 * @param out      结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功
 */
rt_err_t mqtt_do_disconnect(char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    rt_bool_t was_started = mqtt_tool_is_started();
    rt_bool_t was_connected = s_client.isconnected;
    int topics = mqtt_tool_topic_count();

    if (!was_started)
    {
        buf_append(out, out_size, &used,
                   "disconnect: client already stopped (state=%s), nothing to do",
                   mqtt_tool_conn_state());
        return RT_EOK;
    }

    mqtt_tool_stop();

    buf_append(out, out_size, &used,
               "disconnect ok: was_connected=%d, state=%s\n"
               "%d subscription(s) kept - they will be re-subscribed automatically after the next "
               "mqtt_connect (use mqtt_subscribe action=unsubscribe to drop one)",
               was_connected ? 1 : 0, mqtt_tool_conn_state(), topics);
    return RT_EOK;
}

/*
 * @brief 发布消息到指定话题
 * @param topic      目标话题（NULL 使用默认发布话题）
 * @param message    消息内容
 * @param out        结果文本
 * @param out_size   结果缓冲大小
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_do_publish(const char *topic, const char *message, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    rt_size_t limit;
    rt_size_t len;
    const char *pub_topic = (topic != RT_NULL && topic[0] != '\0') ? topic : MQTT_PUBTOPIC;

    if (message == RT_NULL || message[0] == '\0')
    {
        buf_append(out, out_size, &used, "error: empty message");
        return RT_ERROR;
    }
    if (rt_strlen(pub_topic) >= MQTT_TOOL_TOPIC_MAX_LEN)
    {
        buf_append(out, out_size, &used, "error: topic too long (max %d)", MQTT_TOOL_TOPIC_MAX_LEN - 1);
        return RT_ERROR;
    }

    /* 连接前置检查：未启动则启动，未连上则等待并说明原因
       （长度上限依赖 client.buf_size，必须在客户端就绪后再计算） */
    if (!mqtt_tool_ensure_connected(out, out_size))
    {
        buf_append(out, out_size, &used, "\npublish aborted (state=%s)", mqtt_tool_conn_state());
        return RT_ERROR;
    }

    limit = mqtt_publish_limit(pub_topic);
    len = rt_strlen(message);
    if (len > limit)
    {
        buf_append(out, out_size, &used,
                   "\nerror: payload too long (%d bytes, max %d for topic '%s')",
                   (int)len, (int)limit, pub_topic);
        return RT_ERROR;
    }

    if (paho_mqtt_publish(&s_client, MQTT_TOOL_QOS, pub_topic, message) != PAHO_SUCCESS)
    {
        buf_append(out, out_size, &used,
                   "\nerror: publish failed (broker=%s topic=%s state=%s)",
                   MQTT_SERVER_URI, pub_topic, mqtt_tool_conn_state());
        return RT_ERROR;
    }

    buf_append(out, out_size, &used,
               "\npublish ok: topic=%s bytes=%d qos=1 (state=%s)",
               pub_topic, (int)len, mqtt_tool_conn_state());
    return RT_EOK;
}

/*
 * @brief 订阅话题并等待生效（同时可切换已订阅话题的投递模式）
 * @param topic    话题过滤器
 * @param mode     投递模式（filter / auto / poll）
 * @param out      结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_do_subscribe(const char *topic, mqtt_deliver_mode_t mode, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    mqtt_sub_entry_t *entry;
    rt_bool_t active = RT_FALSE;
    rt_bool_t connected = RT_FALSE;
    rt_int32_t waited = 0;

    if (topic == RT_NULL || topic[0] == '\0')
    {
        buf_append(out, out_size, &used, "error: topic is required");
        return RT_ERROR;
    }

    /* 连接前置检查：未启动则启动，未连上则等待（订阅意图仍会登记，连上后自动生效） */
    connected = mqtt_tool_ensure_connected(out, out_size);
    buf_append(out, out_size, &used, "\n");

    if (sub_register(topic, mode) != RT_EOK)
    {
        buf_append(out, out_size, &used,
                   "error: subscribe failed (topic invalid or max %d topics reached)",
                   MQTT_TOOL_MAX_SUB_TOPICS);
        return RT_ERROR;
    }

    /* 等待连接与订阅生效（接收线程每 200ms 同步一次订阅状态） */
    while (waited < PKG_AGENT_TOOL_MQTT_SUB_WAIT_MS)
    {
        if (lock_take())
        {
            entry = sub_find(topic);
            active = (entry != RT_NULL && entry->subscribed) ? RT_TRUE : RT_FALSE;
            lock_give();
        }
        if (active)
        {
            break;
        }
        rt_thread_mdelay(MQTT_TOOL_POLL_STEP_MS);
        waited += MQTT_TOOL_POLL_STEP_MS;
    }

    if (active)
    {
        buf_append(out, out_size, &used, "subscribe ok: topic=%s mode=%s (state=%s)",
                   topic, mqtt_mode_to_string(mode), mqtt_tool_conn_state());
    }
    else if (connected)
    {
        buf_append(out, out_size, &used, "subscribe pending: topic=%s mode=%s (client connected, retrying)",
                   topic, mqtt_mode_to_string(mode));
    }
    else
    {
        buf_append(out, out_size, &used,
                   "subscribe pending: topic=%s mode=%s (mqtt not connected yet, will subscribe automatically)",
                   topic, mqtt_mode_to_string(mode));
    }

    if (mode == MQTT_DELIVER_FILTER)
    {
        buf_append(out, out_size, &used,
                   "\nmode=filter (default): messages are NOT sent to you unless they look like a "
                   "question or match a threshold rule; set one with mqtt_rule. "
                   "Use mode=auto to forward every message, or mode=poll to only buffer them.");
    }
    else if (mode == MQTT_DELIVER_AUTO)
    {
        buf_append(out, out_size, &used,
                   "\nmode=auto: every message is delivered to you for analysis.");
    }
    else
    {
        buf_append(out, out_size, &used,
                   "\nmode=poll: messages are buffered; call mqtt_receive to fetch them.");
    }

    /* 订阅已登记即视为受理成功（连接建立后自动生效） */
    return RT_EOK;
}

/*
 * @brief 退订话题（topic 为空表示全部退订）
 * @param topic    话题过滤器
 * @param out      结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_do_unsubscribe(const char *topic, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    int i;
    int count = 0;

    if (!lock_take())
    {
        buf_append(out, out_size, &used, "error: mqtt state busy, retry later");
        return RT_ERROR;
    }

    if (topic == RT_NULL || topic[0] == '\0')
    {
        for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
        {
            if (s_subs[i].topic[0] != '\0' && s_subs[i].want)
            {
                s_subs[i].want = RT_FALSE;
                count++;
            }
        }
        lock_give();
        buf_append(out, out_size, &used, "unsubscribe all: %d topic(s) marked", count);
        return RT_EOK;
    }

    {
        mqtt_sub_entry_t *entry = sub_find(topic);
        if (entry == RT_NULL)
        {
            lock_give();
            buf_append(out, out_size, &used, "error: topic '%s' is not subscribed", topic);
            return RT_ERROR;
        }
        entry->want = RT_FALSE;
    }
    lock_give();

    buf_append(out, out_size, &used, "unsubscribe ok: topic=%s", topic);
    return RT_EOK;
}

/*
 * @brief 生成订阅列表与运行状态文本
 * @param out      结果文本
 * @param out_size 结果缓冲大小
 */
void mqtt_do_list(char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    int i;
    int active = 0;

    buf_append(out, out_size, &used,
               "client: started=%d connected=%d broker=%s client_id=%s",
               s_started ? 1 : 0, s_client.isconnected ? 1 : 0, MQTT_SERVER_URI, MQTT_CLIENTID);
    buf_append(out, out_size, &used,
               "\ndefault topics: pub=%s sub=%s qos=1", MQTT_PUBTOPIC, MQTT_SUBTOPIC);

    if (!lock_take())
    {
        buf_append(out, out_size, &used, "\nwarning: mqtt state busy, subscription table unavailable");
        return;
    }

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] == '\0')
        {
            continue;
        }
        buf_append(out, out_size, &used,
                   "\n[%d] topic=%s want=%d mode=%s registered=%d subscribed=%d rx=%d injected=%d suppressed=%d",
                   i, s_subs[i].topic, s_subs[i].want ? 1 : 0,
                   mqtt_mode_to_string(s_subs[i].mode),
                   s_subs[i].registered ? 1 : 0, s_subs[i].subscribed ? 1 : 0,
                   (int)s_subs[i].rx_count, (int)s_subs[i].inject_count,
                   (int)s_subs[i].suppressed_count);
        if (s_subs[i].last_payload[0] != '\0')
        {
            buf_append(out, out_size, &used, "\n    last payload: %s", s_subs[i].last_payload);
        }
        if (s_subs[i].want)
        {
            active++;
        }
    }

    buf_append(out, out_size, &used, "\nrules (forward only when matched):");
    for (i = 0; i < MQTT_TOOL_MAX_RULES; i++)
    {
        if (!s_rules[i].used)
        {
            continue;
        }
        buf_append(out, out_size, &used,
                   "\n[%d] topic=%s %s%s %s %g triggered_now=%d alert_count=%d",
                   i, s_rules[i].topic,
                   (s_rules[i].field[0] != '\0') ? s_rules[i].field : "payload",
                   (s_rules[i].field[0] != '\0') ? " (json path)" : "",
                   s_rules[i].op, s_rules[i].value,
                   s_rules[i].triggered ? 1 : 0, (int)s_rules[i].alert_count);
    }

    buf_append(out, out_size, &used,
               "\nsummary: active=%d/%d rx_total=%d injected_total=%d suppressed_total=%d "
               "alerts=%d dropped=%d buffered=%d overwritten=%d",
               active, MQTT_TOOL_MAX_SUB_TOPICS, (int)s_rx_total, (int)s_inject_total,
               (int)s_suppressed_total, (int)s_alert_total,
               (int)s_rx_dropped, s_poll_count, (int)s_poll_overwrites);
    lock_give();
}

int mqtt_do_receive(int max, const char *topic_filter, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    mqtt_rx_item_t item;
    int n = 0;
    int remain;
    int i;

    if (max <= 0 || max > MQTT_TOOL_POLL_DEPTH)
    {
        max = MQTT_TOOL_POLL_DEPTH;
    }

    if (!lock_take())
    {
        buf_append(out, out_size, &used, "error: mqtt state busy, retry later\n");
        return 0;
    }

    if (s_poll_count == 0)
    {
        buf_append(out, out_size, &used,
                   "no buffered mqtt message (rx_total=%d, injected=%d, dropped=%d)",
                   (int)s_rx_total, (int)s_inject_total, (int)s_rx_dropped);
        lock_give();
        return 0;
    }

    /*
     * 在锁内原地整理环形缓冲：匹配项取出，未匹配项原地放回队尾（保持原顺序），
     * 避免额外的大数组开销（本函数也会在 MSH 线程的较小栈上执行）
     */
    remain = s_poll_count;
    for (i = 0; i < remain; i++)
    {
        rt_bool_t matched = RT_TRUE;

        item = s_poll_ring[s_poll_head];
        s_poll_head = (s_poll_head + 1) % MQTT_TOOL_POLL_DEPTH;
        s_poll_count--;

        if (topic_filter != RT_NULL && topic_filter[0] != '\0')
        {
            matched = has_wildcard(topic_filter)
                      ? topic_filter_match(topic_filter, item.topic)
                      : (rt_strcmp(topic_filter, item.topic) == 0);
        }

        if (!matched)
        {
            s_poll_ring[s_poll_tail] = item;
            s_poll_tail = (s_poll_tail + 1) % MQTT_TOOL_POLL_DEPTH;
            s_poll_count++;
            continue;
        }

        buf_append(out, out_size, &used, "[%d] topic: %s\npayload: %s%s\n",
                   n, item.topic, item.payload,
                   item.truncated ? " (truncated, payload longer than buffer)" : "");
        n++;

        if (n >= max)
        {
            /* 达到请求条数：剩余消息保持原顺序留在缓冲中 */
            break;
        }
    }

    if (n == 0)
    {
        buf_append(out, out_size, &used, "no buffered mqtt message matching '%s'\n",
                   (topic_filter != RT_NULL) ? topic_filter : "");
    }
    buf_append(out, out_size, &used, "buffered=%d rx_total=%d injected=%d dropped=%d\n",
               s_poll_count, (int)s_rx_total, (int)s_inject_total, (int)s_rx_dropped);

    lock_give();

    return n;
}

/* 近期汇总里每个数值字段的统计 */
#define MQTT_HIST_STATS_FIELDS  6

typedef struct
{
    char name[MQTT_TOOL_FIELD_MAX_LEN];
    int count;
    double min;
    double max;
    double sum;
    double last;
} mqtt_hist_stat_t;

/*
 * @brief 把一条 payload 里的数值字段累加到统计表
 * @note  对象型 payload 按字段分别统计；整条为数值时按 "payload" 统计
 * @param payload 报文内容
 * @param stats   统计表
 * @param count   统计表已用条目数
 */
static void hist_accumulate(const char *payload, mqtt_hist_stat_t *stats, int *count)
{
    cJSON *root;
    cJSON *item;

    if (payload == RT_NULL || payload[0] == '\0' || stats == RT_NULL || count == RT_NULL)
    {
        return;
    }

    root = cJSON_Parse(payload);
    if (root == RT_NULL)
    {
        return;
    }

    /* 统计表内按字段名查找/新建，并更新 min/max/sum/last */
#define HIST_UPDATE(_name, _value)                                                     \
    do {                                                                               \
        int _i;                                                                        \
        mqtt_hist_stat_t *_s = RT_NULL;                                                \
        for (_i = 0; _i < *count; _i++) {                                              \
            if (rt_strcmp(stats[_i].name, (_name)) == 0) { _s = &stats[_i]; break; }   \
        }                                                                              \
        if (_s == RT_NULL && *count < MQTT_HIST_STATS_FIELDS) {                        \
            _s = &stats[*count];                                                       \
            rt_strncpy(_s->name, (_name), MQTT_TOOL_FIELD_MAX_LEN - 1);                \
            _s->name[MQTT_TOOL_FIELD_MAX_LEN - 1] = '\0';                              \
            _s->count = 0;                                                             \
            _s->min = (_value);                                                        \
            _s->max = (_value);                                                        \
            _s->sum = 0.0;                                                             \
            (*count)++;                                                                \
        }                                                                              \
        if (_s != RT_NULL) {                                                           \
            if ((_value) < _s->min) _s->min = (_value);                               \
            if ((_value) > _s->max) _s->max = (_value);                               \
            _s->sum += (_value);                                                       \
            _s->last = (_value);                                                       \
            _s->count++;                                                               \
        }                                                                              \
    } while (0)

    if (cJSON_IsNumber(root))
    {
        HIST_UPDATE("payload", root->valuedouble);
    }
    else if (cJSON_IsObject(root))
    {
        for (item = root->child; item != RT_NULL; item = item->next)
        {
            double value = 0.0;
            /* 与阈值规则保持同一套取值规则：数值 / 数值字符串 / 布尔 */
            if (item->string != RT_NULL && mqtt_json_node_number(item, &value))
            {
                HIST_UPDATE(item->string, value);
            }
        }
    }

#undef HIST_UPDATE

    cJSON_Delete(root);
}

/*
 * @brief 汇总某话题（或全部话题）的近期情况：最近消息（含 payload）+ 数值字段统计 + 计数
 * @param topic_filter 话题过滤器（NULL/空表示全部）
 * @param max          每个话题最多列出多少条近期消息
 * @param out          结果文本
 * @param out_size     结果缓冲大小
 * @return 汇总到的话题数
 */
int mqtt_do_history(const char *topic_filter, int max, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    int i;
    int topics = 0;
    rt_tick_t now;

    if (max <= 0 || max > MQTT_TOOL_HISTORY_DEPTH)
    {
        max = MQTT_TOOL_HISTORY_DEPTH;
    }

    if (!lock_take())
    {
        buf_append(out, out_size, &used, "error: mqtt state busy, retry later\n");
        return 0;
    }

    now = rt_tick_get();

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        mqtt_sub_entry_t *entry = &s_subs[i];
        mqtt_hist_stat_t stats[MQTT_HIST_STATS_FIELDS];
        int stat_count = 0;
        int listed = 0;
        int k;

        if (entry->topic[0] == '\0')
        {
            continue;
        }
        if (topic_filter != RT_NULL && topic_filter[0] != '\0')
        {
            rt_bool_t matched = has_wildcard(topic_filter)
                                ? topic_filter_match(topic_filter, entry->topic)
                                : (rt_strcmp(topic_filter, entry->topic) == 0);
            if (!matched)
            {
                continue;
            }
        }

        topics++;

        buf_append(out, out_size, &used,
                   "\n[topic=%s] mode=%s want=%d received=%d forwarded=%d filtered=%d history=%d\n",
                   entry->topic, mqtt_mode_to_string(entry->mode), entry->want ? 1 : 0,
                   (int)entry->rx_count, (int)entry->inject_count,
                   (int)entry->suppressed_count, (int)entry->hist_count);

        if (entry->hist_count == 0)
        {
            buf_append(out, out_size, &used, "  (no message recorded yet)\n");
            continue;
        }

        /* 从最旧到最新列出最近消息，并顺带累计数值统计 */
        buf_append(out, out_size, &used, "  recent messages (oldest -> newest):\n");
        for (k = 0; k < (int)entry->hist_count; k++)
        {
            rt_uint8_t idx = (rt_uint8_t)((entry->hist_head + k) % MQTT_TOOL_HISTORY_DEPTH);
            const mqtt_hist_item_t *h = &entry->hist[idx];
            rt_int32_t age_s = (rt_int32_t)((now - h->tick) / RT_TICK_PER_SECOND);

            hist_accumulate(h->payload, stats, &stat_count);

            if (listed < max)
            {
                buf_append(out, out_size, &used, "    %d) %lds ago: %s%s\n",
                           listed + 1, (long)age_s, h->payload,
                           h->truncated ? " (truncated)" : "");
                listed++;
            }
        }
        if ((int)entry->hist_count > listed)
        {
            buf_append(out, out_size, &used, "    ... %d older message(s) not listed\n",
                       (int)entry->hist_count - listed);
        }

        if (stat_count > 0)
        {
            buf_append(out, out_size, &used, "  numeric summary:\n");
            for (k = 0; k < stat_count; k++)
            {
                buf_append(out, out_size, &used,
                           "    %s: n=%d min=%g max=%g avg=%g last=%g\n",
                           stats[k].name, stats[k].count, stats[k].min, stats[k].max,
                           (stats[k].count > 0) ? (stats[k].sum / stats[k].count) : 0.0,
                           stats[k].last);
            }
        }
    }

    if (topics == 0)
    {
        /*
         * 没有任何匹配的订阅：明确告诉模型「查不到」的原因和下一步动作，
         * 并明确要求不要再重复调用，否则模型会以为没拿到数据而无限重试。
         */
        buf_append(out, out_size, &used,
                   "no history for topic '%s': it matches 0 subscribed topic(s).\n"
                   "reason: the topic was never subscribed in this session "
                   "(client started=%d, connected=%d), so no message could be recorded for it.\n"
                   "next step: subscribe it with mqtt_subscribe (or just report to the user that "
                   "there is no data yet). Do NOT call mqtt_history again with the same topic - "
                   "the result will not change.\n",
                   (topic_filter != RT_NULL && topic_filter[0] != '\0') ? topic_filter : "*",
                   s_started ? 1 : 0, (s_client.isconnected != 0) ? 1 : 0);
    }

    buf_append(out, out_size, &used,
               "\ncounters: rx_total=%d forwarded=%d filtered=%d alerts=%d\n"
               "note: this view is read-only; use mqtt_receive to consume buffered messages",
               (int)s_rx_total, (int)s_inject_total, (int)s_suppressed_total, (int)s_alert_total);

    lock_give();
    return topics;
}

