/*
 * MQTT 工具 - 引擎核心（状态、基础工具、paho 回调、缓冲与消息注入）
 * 维护客户端句柄/订阅表/规则表/统计等全局状态，提供加解锁、话题匹配、
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#include <stdarg.h>

/* ==========================================================================
 * 模块状态
 * ========================================================================== */

MQTTClient s_client;
rt_bool_t s_started = RT_FALSE;

/* 订阅表 + 规则表 + 轮询缓冲 + 统计：由 s_lock 保护 */
mqtt_sub_entry_t s_subs[MQTT_TOOL_MAX_SUB_TOPICS];
mqtt_rule_entry_t s_rules[MQTT_TOOL_MAX_RULES];
mqtt_rx_item_t s_poll_ring[MQTT_TOOL_POLL_DEPTH];
int s_poll_head = 0;
int s_poll_tail = 0;
int s_poll_count = 0;
rt_uint32_t s_rx_total = 0;
rt_uint32_t s_rx_dropped = 0;
rt_uint32_t s_poll_overwrites = 0;
rt_uint32_t s_inject_total = 0;
rt_uint32_t s_suppressed_total = 0;
rt_uint32_t s_alert_total = 0;
rt_mutex_t s_lock = RT_NULL;

/* 接收队列与接收线程 */
rt_mq_t s_rx_queue = RT_NULL;
rt_thread_t s_rx_thread = RT_NULL;
volatile rt_bool_t s_rx_running = RT_FALSE;
rt_sem_t s_rx_exit_sem = RT_NULL;
rt_tick_t s_last_inject_tick = 0;

/* 以下两个缓冲仅由 paho 工作线程（回调）与接收线程各自访问，无需加锁 */
static mqtt_rx_item_t s_scratch;                                     /* 回调组包暂存 */
/* 回调去重：重叠过滤器（如 a/# 与 a/b）会让 paho 对同一报文回调多次 */
static mqtt_rx_item_t s_last_item;
static rt_uint16_t s_last_id = 0;
rt_bool_t s_has_last = RT_FALSE;

/* ==========================================================================
 * 基础工具函数
 * ========================================================================== */

/*
 * @brief 创建状态锁（幂等）
 * @return RT_TRUE 锁可用
 * @note  锁必须在「客户端启动之前」就存在：mqtt_tool status / mqtt_history /
 *        mqtt_receive 等只读操作在未启动时也要读订阅表（历史、计数、投递模式），
 *        否则这些命令一律误报 "error: mqtt state busy, retry later"。
 *        锁一经创建就不再释放（mqtt_tool_stop 不删锁），因此可以安全地懒创建。
 */
rt_bool_t mqtt_tool_lock_init(void)
{
    if (s_lock == RT_NULL)
    {
        s_lock = rt_mutex_create("mqtt_lock", RT_IPC_FLAG_PRIO);
        if (s_lock == RT_NULL)
        {
            LOG_E("create mqtt mutex failed");
            return RT_FALSE;
        }
    }
    return RT_TRUE;
}

/*
 * @brief 获取状态锁（带超时，绝不无限等待）
 * @return RT_TRUE 成功；RT_FALSE 超时/锁创建失败
 * @note  临界区只允许纯内存操作：paho 收发、日志输出一律放到锁外，
 *        否则一旦底层 socket/console 阻塞，会把 agent 工具调用与 MSH 一起卡死
 */
rt_bool_t lock_take(void)
{
    /* 未启动客户端时也要能读订阅表：锁不存在则先创建（幂等） */
    if (s_lock == RT_NULL && !mqtt_tool_lock_init())
    {
        return RT_FALSE;
    }
    return (rt_mutex_take(s_lock, rt_tick_from_millisecond(PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS)) == RT_EOK)
           ? RT_TRUE : RT_FALSE;
}

/*
 * @brief 释放状态锁（与 lock_take 配对）
 */
void lock_give(void)
{
    if (s_lock != RT_NULL)
    {
        rt_mutex_release(s_lock);
    }
}

/*
 * @brief ticks 转毫秒（RT-Thread 5.x 未提供 rt_tick_to_ms）
 * @param ticks tick 数
 * @return 毫秒数
 */
rt_int32_t ticks_to_ms(rt_tick_t ticks)
{
    return (rt_int32_t)((rt_uint32_t)ticks * 1000U / RT_TICK_PER_SECOND);
}

/*
 * @brief 字符串中是否包含 MQTT 通配符（+ 或 #）
 * @param str 待检查字符串
 * @return RT_TRUE 含通配符
 */
rt_bool_t has_wildcard(const char *str)
{
    const char *p;

    if (str == RT_NULL)
    {
        return RT_FALSE;
    }

    for (p = str; *p != '\0'; p++)
    {
        if (*p == '+' || *p == '#')
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

/*
 * @brief 把一条消息追加进订阅记录的近期历史环形缓冲
 * @note  调用者需持有 s_lock；满时覆盖最旧一条，始终保留最近 N 条
 * @param entry 订阅记录
 * @param item  消息
 */
void mqtt_hist_push(mqtt_sub_entry_t *entry, const mqtt_rx_item_t *item)
{
    mqtt_hist_item_t *slot;
    rt_uint8_t idx;

    if (entry == RT_NULL || item == RT_NULL)
    {
        return;
    }

    if (entry->hist_count < MQTT_TOOL_HISTORY_DEPTH)
    {
        idx = (rt_uint8_t)((entry->hist_head + entry->hist_count) % MQTT_TOOL_HISTORY_DEPTH);
        entry->hist_count++;
    }
    else
    {
        /* 已满：覆盖最旧一条并前移队头 */
        idx = entry->hist_head;
        entry->hist_head = (rt_uint8_t)((entry->hist_head + 1) % MQTT_TOOL_HISTORY_DEPTH);
    }

    slot = &entry->hist[idx];
    rt_strncpy(slot->payload, item->payload, MQTT_TOOL_HISTORY_PAYLOAD_LEN - 1);
    slot->payload[MQTT_TOOL_HISTORY_PAYLOAD_LEN - 1] = '\0';
    slot->tick = rt_tick_get();
    slot->truncated = item->truncated;
}

/*
 * @brief 投递模式字符串 -> 枚举
 * @param mode 模式字符串（filter / auto / poll），其它值按 filter 处理
 * @return 模式枚举
 */
mqtt_deliver_mode_t mqtt_mode_from_string(const char *mode)
{
    if (mode == RT_NULL)
    {
        return MQTT_DELIVER_FILTER;
    }
    if (rt_strcmp(mode, "auto") == 0)
    {
        return MQTT_DELIVER_AUTO;
    }
    if (rt_strcmp(mode, "poll") == 0)
    {
        return MQTT_DELIVER_POLL;
    }
    return MQTT_DELIVER_FILTER;
}

/*
 * @brief 投递模式枚举 -> 字符串
 * @param mode 模式枚举
 * @return 模式字符串
 */
const char *mqtt_mode_to_string(mqtt_deliver_mode_t mode)
{
    switch (mode)
    {
    case MQTT_DELIVER_AUTO:
        return "auto";
    case MQTT_DELIVER_POLL:
        return "poll";
    default:
        return "filter";
    }
}

/*
 * @brief 安全拼接格式化文本（带边界保护）
 * @param buf  目标缓冲
 * @param size 缓冲总大小
 * @param used 已用长度（输入/输出）
 * @param fmt  格式串
 */
void buf_append(char *buf, rt_size_t size, rt_size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (buf == RT_NULL || used == RT_NULL || fmt == RT_NULL || *used >= size)
    {
        return;
    }

    va_start(ap, fmt);
    n = rt_vsnprintf(buf + *used, size - *used, fmt, ap);
    va_end(ap);

    if (n > 0)
    {
        *used += (rt_size_t)n;
        if (*used >= size)
        {
            *used = size - 1;
        }
    }
}

/*
 * @brief MQTT 话题过滤器匹配（支持 + 与 # 通配）
 * @param filter 订阅过滤器
 * @param topic  实际话题
 * @return RT_TRUE 匹配
 */
rt_bool_t topic_filter_match(const char *filter, const char *topic)
{
    const char *f = filter;
    const char *t = topic;
    rt_bool_t level_start = RT_TRUE;

    if (filter == RT_NULL || topic == RT_NULL)
    {
        return RT_FALSE;
    }

    while (*f != '\0')
    {
        if (level_start && *f == '#')
        {
            /* '#' 表示匹配剩余全部层级（含零层）；"a/b#" 按字面量处理 */
            return RT_TRUE;
        }

        if (level_start && *f == '+')
        {
            /* '+' 匹配本层级任意非空内容 */
            if (*t == '\0' || *t == '/')
            {
                return RT_FALSE;
            }
            while (*t != '\0' && *t != '/')
            {
                t++;
            }
            f++;
        }
        else
        {
            if (*f != *t)
            {
                return RT_FALSE;
            }
            f++;
            t++;
        }
        level_start = RT_FALSE;

        if (*f == '/')
        {
            if (*t == '\0' && *(f + 1) == '#')
            {
                /* "a/#" 也匹配父层级 "a" */
                return RT_TRUE;
            }
            if (*t != '/')
            {
                return RT_FALSE;
            }
            f++;
            t++;
            level_start = RT_TRUE;
        }
        else if (*t == '/')
        {
            /* 过滤器层级已结束，话题还有剩余层级 */
            return RT_FALSE;
        }
    }

    return (*t == '\0') ? RT_TRUE : RT_FALSE;
}

/*
 * @brief 按话题精确查找订阅记录（调用者需持有 s_lock）
 * @param topic 话题字符串
 * @return 记录指针，未找到返回 RT_NULL
 */
mqtt_sub_entry_t *sub_find(const char *topic)
{
    int i;

    if (topic == RT_NULL || topic[0] == '\0')
    {
        return RT_NULL;
    }

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] != '\0' && rt_strcmp(s_subs[i].topic, topic) == 0)
        {
            return &s_subs[i];
        }
    }
    return RT_NULL;
}

/*
 * @brief 按过滤器匹配查找订阅记录（调用者需持有 s_lock）
 * @param topic 收到消息的实际话题
 * @return 首个匹配且处于订阅状态的记录，未找到返回 RT_NULL
 */
mqtt_sub_entry_t *sub_match(const char *topic)
{
    int i;

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] == '\0' || !s_subs[i].want)
        {
            continue;
        }
        if (topic_filter_match(s_subs[i].topic, topic))
        {
            return &s_subs[i];
        }
    }
    return RT_NULL;
}

/*
 * @brief 查找空闲订阅记录槽位（调用者需持有 s_lock）
 * @return 空槽指针，无空闲返回 RT_NULL
 */
mqtt_sub_entry_t *sub_find_free(void)
{
    int i;

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] == '\0')
        {
            return &s_subs[i];
        }
    }
    return RT_NULL;
}

/*
 * @brief paho 回调（运行在 paho 工作线程上下文）
 */

void mqtt_connect_callback(MQTTClient *c)
{
    LOG_I("mqtt connecting: %s", MQTT_SERVER_URI);
}

void mqtt_online_callback(MQTTClient *c)
{
    LOG_I("mqtt online: %s", MQTT_SERVER_URI);
}

void mqtt_offline_callback(MQTTClient *c)
{
    LOG_W("mqtt offline, will retry automatically");
}

/*
 * @brief 订阅消息回调：只做「拷贝 + 入队」，不做耗时处理
 * @note  运行在 paho 工作线程，必须尽快返回，否则会阻塞 keepalive 与 socket 收发；
 *        真正的分析/注入由接收线程完成
 * @param c        客户端句柄
 * @param msg_data 消息数据
 */
void mqtt_sub_callback(MQTTClient *c, MessageData *msg_data)
{
    const char *topic;
    rt_size_t topic_len;
    rt_size_t copy_len;
    rt_size_t raw_len;

    RT_UNUSED(c);

    if (msg_data == RT_NULL || msg_data->message == RT_NULL || msg_data->topicName == RT_NULL)
    {
        return;
    }

    topic = msg_data->topicName->lenstring.data;
    topic_len = (rt_size_t)msg_data->topicName->lenstring.len;
    raw_len = msg_data->message->payloadlen;

    if (topic == RT_NULL || msg_data->message->payload == RT_NULL || s_rx_queue == RT_NULL)
    {
        return;
    }

    /* 组包：topic / payload 均按缓冲区边界截断，不修改 paho 的 readbuf */
    rt_memset(&s_scratch, 0, sizeof(s_scratch));
    copy_len = (topic_len < (MQTT_TOOL_TOPIC_MAX_LEN - 1)) ? topic_len : (MQTT_TOOL_TOPIC_MAX_LEN - 1);
    rt_memcpy(s_scratch.topic, topic, copy_len);
    s_scratch.topic[copy_len] = '\0';

    copy_len = (raw_len < (MQTT_TOOL_PAYLOAD_MAX_LEN - 1)) ? raw_len : (MQTT_TOOL_PAYLOAD_MAX_LEN - 1);
    rt_memcpy(s_scratch.payload, msg_data->message->payload, copy_len);
    s_scratch.payload[copy_len] = '\0';
    s_scratch.payload_len = (rt_uint16_t)copy_len;
    s_scratch.raw_len = (rt_uint16_t)((raw_len > 0xFFFF) ? 0xFFFF : raw_len);
    s_scratch.truncated = (copy_len < raw_len) ? 1 : 0;

    /* 去重：重叠过滤器会对同一报文回调多次 */
    if (s_has_last && s_last_id == msg_data->message->id &&
        s_last_item.payload_len == s_scratch.payload_len &&
        rt_strcmp(s_last_item.topic, s_scratch.topic) == 0 &&
        rt_memcmp(s_last_item.payload, s_scratch.payload, s_scratch.payload_len) == 0)
    {
        return;
    }

    /* 入队（rt_mq_send 在 RT-Thread 5.x 中为非阻塞：队列满立即返回 -RT_EFULL） */
    if (rt_mq_send(s_rx_queue, &s_scratch, sizeof(s_scratch)) != RT_EOK)
    {
        s_rx_dropped++;
        LOG_W("mqtt rx queue full, drop message on topic %s", s_scratch.topic);
        return;
    }

    s_last_item = s_scratch;
    s_last_id = msg_data->message->id;
    s_has_last = RT_TRUE;
}

/* ==========================================================================
 * 轮询缓冲（poll 模式消息暂存）
 * ========================================================================== */

/*
 * @brief 压入轮询缓冲（满时丢弃最旧一条）
 * @param item 待缓存消息
 */
void poll_push(const mqtt_rx_item_t *item)
{
    if (item == RT_NULL)
    {
        return;
    }

    /* 加锁失败说明状态锁异常：宁可丢弃也不在无锁状态下改环形缓冲 */
    if (!lock_take())
    {
        s_rx_dropped++;
        LOG_W("mqtt state lock busy, drop buffered message from %s", item->topic);
        return;
    }

    if (s_poll_count >= MQTT_TOOL_POLL_DEPTH)
    {
        /* 缓冲满：丢弃最旧一条，保证新消息可见（filter 模式下这里是滚动窗口，属正常覆盖） */
        s_poll_head = (s_poll_head + 1) % MQTT_TOOL_POLL_DEPTH;
        s_poll_count--;
        s_poll_overwrites++;
    }

    s_poll_ring[s_poll_tail] = *item;
    s_poll_tail = (s_poll_tail + 1) % MQTT_TOOL_POLL_DEPTH;
    s_poll_count++;

    lock_give();
}

/*
 * @brief 清空轮询缓冲，返回被清掉的消息条数
 * @return 清掉的条数，加锁失败返回 -1
 */
int poll_clear(void)
{
    int n;

    if (!lock_take())
    {
        return -1;
    }

    n = s_poll_count;
    s_poll_count = 0;
    s_poll_head = 0;
    s_poll_tail = 0;

    lock_give();
    return n;
}

/*
 * 注：向 agent 注入文本的能力已提升为框架公共 API ——
 *     agent_inject_text()（声明见 include/utils.h，实现见 src/utils.c），
 *     MQTT 模块（tool_mqtt_route.c）与其它工具共用同一入口；
 *     MQTT 特有的注入计数/错峰时间戳留在本模块里维护。
 */

