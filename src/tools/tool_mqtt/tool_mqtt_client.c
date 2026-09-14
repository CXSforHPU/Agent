/*
 * MQTT 工具 - 连接与订阅生命周期（订阅表同步、启动/停止、订阅生效确认）
 * 负责把订阅意图落实到 paho 槽位、管理客户端与接收线程的启动/停止。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* ==========================================================================
 * 订阅状态同步
 * ========================================================================== */

/*
 * @brief 依据 paho 实际槽位内容刷新 registered 标志
 * @note  paho_mqtt_subscribe/unsubscribe 可能分配到「非预期槽位」
 *        （其内部按前缀匹配），调用后必须以实际槽位为准
 */
void subs_sync_registered(void)
{
    int i;
    int j;

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        s_subs[i].registered = RT_FALSE;
    }

    for (i = 0; i < MAX_MESSAGE_HANDLERS; i++)
    {
        const char *filter = s_client.messageHandlers[i].topicFilter;

        if (filter == RT_NULL)
        {
            continue;
        }
        for (j = 0; j < MQTT_TOOL_MAX_SUB_TOPICS; j++)
        {
            if (s_subs[j].topic[0] != '\0' && rt_strcmp(s_subs[j].topic, filter) == 0)
            {
                s_subs[j].registered = RT_TRUE;
                break;
            }
        }
    }
}

/*
 * @brief 把订阅表的意图（want）落实到 paho
 * @note  分三步：持锁取快照 -> 锁外调用 paho -> 持锁回写状态。
 *        paho_mqtt_subscribe/unsubscribe 会直接做 socket 收发（send/select 阻塞时间
 *        不可控），绝不能在持锁时调用，否则会把 agent 工具调用与 MSH 一起卡住。
 *        每轮最多处理一个话题，避免一次同步长时间占用接收线程。
 *        仅在「已连接」时改动 paho 槽位，避免与工作线程的连接/订阅流程竞态。
 */
void subs_sync_with_client(void)
{
    char topic[MQTT_TOOL_TOPIC_MAX_LEN];
    char prereg_topic[MQTT_TOOL_TOPIC_MAX_LEN];
    rt_bool_t prereg_found = RT_FALSE;
    rt_bool_t do_subscribe = RT_FALSE;
    rt_bool_t pending = RT_FALSE;
    rt_bool_t rc_ok = RT_FALSE;
    int i;

    if (!s_client.isconnected)
    {
        return;
    }

    topic[0] = '\0';
    prereg_topic[0] = '\0';

    /* 1. 快照：找出一个待落地的操作 */
    if (!lock_take())
    {
        return;
    }

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        mqtt_sub_entry_t *entry = &s_subs[i];

        if (entry->topic[0] == '\0')
        {
            continue;
        }

        if (entry->want && !entry->registered)
        {
            rt_strncpy(topic, entry->topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
            topic[MQTT_TOOL_TOPIC_MAX_LEN - 1] = '\0';
            do_subscribe = RT_TRUE;
            pending = RT_TRUE;
            break;
        }

        if (entry->want && !entry->subscribed)
        {
            /* 启动前预注册的槽位：工作线程已在连接成功后订阅
               （日志与 paho 调用一样放在锁外，避免 console 阻塞时持锁） */
            entry->subscribed = RT_TRUE;
            rt_strncpy(prereg_topic, entry->topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
            prereg_topic[MQTT_TOOL_TOPIC_MAX_LEN - 1] = '\0';
            prereg_found = RT_TRUE;
            continue;
        }

        if (!entry->want && entry->registered)
        {
            rt_strncpy(topic, entry->topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
            topic[MQTT_TOOL_TOPIC_MAX_LEN - 1] = '\0';
            do_subscribe = RT_FALSE;
            pending = RT_TRUE;
            break;
        }
    }

    lock_give();

    if (prereg_found)
    {
        LOG_I("mqtt subscribe active (pre-registered): %s", prereg_topic);
    }

    if (!pending)
    {
        return;
    }

    /* 2. 锁外调用 paho（可能阻塞在内核 socket 收发） */
    if (do_subscribe)
    {
        rc_ok = (paho_mqtt_subscribe(&s_client, MQTT_TOOL_QOS, topic, mqtt_sub_callback) == PAHO_SUCCESS)
                ? RT_TRUE : RT_FALSE;
        LOG_I("mqtt subscribe %s: %s", rc_ok ? "ok" : "failed", topic);
    }
    else
    {
        rc_ok = (paho_mqtt_unsubscribe(&s_client, topic) == PAHO_SUCCESS) ? RT_TRUE : RT_FALSE;
        LOG_I("mqtt unsubscribe %s: %s", rc_ok ? "ok" : "failed", topic);
    }

    /* 3. 回写：以 paho 实际槽位为准（其内部按前缀匹配，可能落到别的槽位） */
    if (!lock_take())
    {
        return;
    }

    subs_sync_registered();

    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] != '\0' && rt_strcmp(s_subs[i].topic, topic) == 0)
        {
            if (do_subscribe)
            {
                s_subs[i].subscribed = rc_ok ? RT_TRUE : RT_FALSE;
            }
            else if (rc_ok)
            {
                s_subs[i].subscribed = RT_FALSE;
            }
            break;
        }
    }

    lock_give();
}

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/*
 * @brief 登记一条订阅（仅维护内部表与 paho 槽位，不等待结果）
 * @param topic 话题过滤器
 * @param mode  投递模式（filter / auto / poll）
 * @return RT_EOK 成功，RT_ERROR 失败（参数非法或订阅表已满）
 */
rt_err_t sub_register(const char *topic, mqtt_deliver_mode_t mode)
{
    mqtt_sub_entry_t *entry;
    rt_err_t ret = RT_EOK;

    if (topic == RT_NULL || topic[0] == '\0' || rt_strlen(topic) >= MQTT_TOOL_TOPIC_MAX_LEN)
    {
        return RT_ERROR;
    }

    if (!lock_take())
    {
        return RT_ERROR;
    }

    entry = sub_find(topic);
    if (entry == RT_NULL)
    {
        entry = sub_find_free();
        if (entry == RT_NULL)
        {
            ret = RT_ERROR;
        }
        else
        {
            rt_memset(entry, 0, sizeof(*entry));
            rt_strncpy(entry->topic, topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
            entry->topic[MQTT_TOOL_TOPIC_MAX_LEN - 1] = '\0';
            entry->want = RT_TRUE;
            entry->mode = mode;
            entry->registered = RT_FALSE;
            entry->subscribed = RT_FALSE;

            /* 客户端尚未启动/未连接时预注册槽位，使工作线程在连接成功后自动订阅 */
            if (!s_client.isconnected && s_started == RT_FALSE)
            {
                int i;
                for (i = 0; i < MAX_MESSAGE_HANDLERS; i++)
                {
                    if (s_client.messageHandlers[i].topicFilter == RT_NULL)
                    {
                        s_client.messageHandlers[i].topicFilter = rt_strdup(entry->topic);
                        if (s_client.messageHandlers[i].topicFilter != RT_NULL)
                        {
                            s_client.messageHandlers[i].qos = MQTT_TOOL_QOS;
                            s_client.messageHandlers[i].callback = mqtt_sub_callback;
                            entry->registered = RT_TRUE;
                        }
                        break;
                    }
                }
            }
        }
    }
    else
    {
        entry->want = RT_TRUE;
        entry->mode = mode;
    }

    lock_give();

    return ret;
}

/*
 * @brief 启动 MQTT 客户端（幂等）
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_tool_start(void)
{
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    if (s_started)
    {
        return RT_EOK;
    }

    /* 1. 状态锁（幂等；未启动时的只读操作也会自行创建） */
    if (!mqtt_tool_lock_init())
    {
        return RT_ERROR;
    }

    /* 2. 接收队列（回调 -> 接收线程） */
    if (s_rx_queue == RT_NULL)
    {
        s_rx_queue = rt_mq_create("mqtt_rx", sizeof(mqtt_rx_item_t),
                                  PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
        if (s_rx_queue == RT_NULL)
        {
            LOG_E("create mqtt rx queue failed");
            return RT_ERROR;
        }
    }

    /* 3. paho 客户端配置（工作线程尚未启动，此阶段可安全预注册订阅槽位） */
    rt_memset(&s_client, 0, sizeof(s_client));
    s_client.uri = MQTT_SERVER_URI;

    rt_memcpy(&s_client.condata, &condata, sizeof(condata));
    /* 字符串字面量具备静态存储期，paho 重连时会长期引用，不可使用临时缓冲 */
    s_client.condata.clientID.cstring = MQTT_CLIENTID;
    s_client.condata.keepAliveInterval = PKG_AGENT_TOOL_MQTT_KEEPALIVE_SEC;
    s_client.condata.cleansession = 1;
    s_client.condata.username.cstring = MQTT_USERNAME;
    s_client.condata.password.cstring = MQTT_PASSWORD;

    /* 遗嘱消息：异常掉线时由 broker 代发 */
    s_client.condata.willFlag = 1;
    s_client.condata.will.qos = (char)MQTT_TOOL_QOS;
    s_client.condata.will.retained = 0;
    s_client.condata.will.topicName.cstring = MQTT_PUBTOPIC;
    s_client.condata.will.message.cstring = MQTT_WILLMSG;

    s_client.buf_size = s_client.readbuf_size = MQTT_PUB_SUB_BUF_SIZE;
    s_client.buf = rt_calloc(1, s_client.buf_size);
    s_client.readbuf = rt_calloc(1, s_client.readbuf_size);
    if (s_client.buf == RT_NULL || s_client.readbuf == RT_NULL)
    {
        LOG_E("no memory for mqtt client buffer");
        if (s_client.buf) rt_free(s_client.buf);
        if (s_client.readbuf) rt_free(s_client.readbuf);
        s_client.buf = RT_NULL;
        s_client.readbuf = RT_NULL;
        return RT_ERROR;
    }

    s_client.connect_callback = mqtt_connect_callback;
    s_client.online_callback = mqtt_online_callback;
    s_client.offline_callback = mqtt_offline_callback;
    s_client.defaultMessageHandler = mqtt_sub_callback;

    /* 预注册默认订阅话题（默认 filter 模式：只有咨询/命中阈值规则才交给 agent） */
#if MQTT_TOOL_AUTO_SUB_DEFAULT
    if (sub_register(MQTT_SUBTOPIC, mqtt_mode_from_string(PKG_AGENT_TOOL_MQTT_DEFAULT_MODE)) != RT_EOK)
    {
        LOG_W("register default sub topic '%s' failed", MQTT_SUBTOPIC);
    }
#endif

    /* 4. 启动 paho 工作线程（内部完成连接、订阅、keepalive、收发） */
    if (paho_mqtt_start(&s_client) != PAHO_SUCCESS)
    {
        LOG_E("paho_mqtt_start failed");
        return RT_ERROR;
    }

    /* 5. 启动接收线程 */
    if (s_rx_exit_sem == RT_NULL)
    {
        s_rx_exit_sem = rt_sem_create("mqtt_exit", 0, RT_IPC_FLAG_FIFO);
    }
    s_rx_running = RT_TRUE;
    s_rx_thread = rt_thread_create("mqtt_rx", mqtt_rx_thread, RT_NULL,
                                   PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK,
                                   PKG_AGENT_TOOL_MQTT_RX_THREAD_PRIO, 10);
    if (s_rx_thread == RT_NULL)
    {
        LOG_E("create mqtt rx thread failed");
        s_rx_running = RT_FALSE;
        paho_mqtt_stop(&s_client);
        return RT_ERROR;
    }
    rt_thread_startup(s_rx_thread);

    s_started = RT_TRUE;
    LOG_I("mqtt tool started: broker=%s client_id=%s", MQTT_SERVER_URI, MQTT_CLIENTID);
    return RT_EOK;
}

/*
 * @brief 停止 MQTT 客户端并释放接收线程/队列（幂等）
 * @note  paho 工作线程仅在「已连接」时才能通过 pipe 命令退出；
 *        未连接时该线程会继续后台重连，属 paho(pipe) 实现限制
 */
void mqtt_tool_stop(void)
{
    int i;
    rt_bool_t rx_exited = RT_TRUE;

    /* 1. 停止接收线程并等待其退出（线程退出前不再访问队列） */
    if (s_rx_thread != RT_NULL)
    {
        s_rx_running = RT_FALSE;
        if (s_rx_exit_sem != RT_NULL)
        {
            if (rt_sem_take(s_rx_exit_sem, rt_tick_from_millisecond(MQTT_TOOL_RX_WAIT_MS * 10)) != RT_EOK)
            {
                rx_exited = RT_FALSE;
                LOG_W("mqtt rx thread not exited yet");
            }
        }
        s_rx_thread = RT_NULL;
    }

    /* 2. 断开并释放 paho 资源（缓冲、pipe、订阅槽位） */
    if (s_client.isconnected)
    {
        paho_mqtt_stop(&s_client);
    }
    else
    {
        LOG_W("mqtt not connected: worker keeps reconnecting, skip graceful stop");
    }

    /* 3. 释放队列/信号量并复位状态（接收线程未确认退出时保留队列，避免访问已释放对象） */
    if (s_rx_queue != RT_NULL && rx_exited)
    {
        rt_mq_delete(s_rx_queue);
        s_rx_queue = RT_NULL;
    }
    if (s_rx_exit_sem != RT_NULL)
    {
        rt_sem_delete(s_rx_exit_sem);
        s_rx_exit_sem = RT_NULL;
    }

    if (lock_take())
    {
        for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
        {
            s_subs[i].registered = RT_FALSE;
            s_subs[i].subscribed = RT_FALSE;
        }
        s_poll_count = 0;
        s_poll_head = 0;
        s_poll_tail = 0;
        lock_give();
    }

    s_has_last = RT_FALSE;
    s_started = RT_FALSE;
    LOG_I("mqtt tool stopped");
}

/*
 * @brief MQTT 客户端是否已启动
 * @return RT_TRUE 已启动
 */
rt_bool_t mqtt_tool_is_started(void)
{
    return s_started;
}

/*
 * @brief 查询当前连接状态文本
 * @return "stopped"（未启动）/ "connecting"（已启动未连上）/ "connected"
 */
const char *mqtt_tool_conn_state(void)
{
    if (!s_started)
    {
        return "stopped";
    }
    return s_client.isconnected ? "connected" : "connecting";
}

/*
 * @brief 确保客户端已启动并已连接（发布/订阅前的统一前置检查）
 * @param out      结果文本（可为 NULL）
 * @param out_size 结果缓冲大小
 * @return RT_TRUE 已连接
 */
rt_bool_t mqtt_tool_ensure_connected(char *out, rt_size_t out_size)
{
    rt_size_t used = 0;

    /* 1. 未启动：先启动（内部会建队列/接收线程并启动 paho 工作线程） */
    if (!s_started)
    {
        if (mqtt_tool_start() != RT_EOK)
        {
            buf_append(out, out_size, &used, "error: mqtt client start failed");
            return RT_FALSE;
        }
        buf_append(out, out_size, &used, "client started, connecting to %s ...\n", MQTT_SERVER_URI);
    }

    /* 2. 已连接：直接返回 */
    if (s_client.isconnected)
    {
        buf_append(out, out_size, &used, "connection ok: broker=%s (already connected)", MQTT_SERVER_URI);
        return RT_TRUE;
    }

    /* 3. 未连接：等待（paho 工作线程在后台每 5s 自动重连） */
    if (mqtt_wait_connected(PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS))
    {
        buf_append(out, out_size, &used, "connection ok: broker=%s", MQTT_SERVER_URI);
        return RT_TRUE;
    }

    buf_append(out, out_size, &used,
               "error: mqtt not connected (broker=%s, waited %d ms); the client keeps reconnecting "
               "in the background - check network/broker address/credentials, then retry or call "
               "mqtt_connect again",
               MQTT_SERVER_URI, (int)PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS);
    return RT_FALSE;
}

/*
 * @brief 等待 MQTT 连接建立
 * @param timeout_ms 超时（毫秒）
 * @return RT_TRUE 已连接
 */
rt_bool_t mqtt_wait_connected(rt_int32_t timeout_ms)
{
    rt_int32_t waited = 0;

    while (waited < timeout_ms)
    {
        if (s_client.isconnected)
        {
            return RT_TRUE;
        }
        rt_thread_mdelay(MQTT_TOOL_POLL_STEP_MS);
        waited += MQTT_TOOL_POLL_STEP_MS;
    }

    return s_client.isconnected ? RT_TRUE : RT_FALSE;
}

/* ==========================================================================
 * 应用层动态增删订阅话题
 * ========================================================================== */

/*
 * @brief 动态增加（或更新）一个订阅话题
 * @param topic 话题过滤器（支持 + / # 通配）
 * @param mode  投递模式（filter / auto / poll）
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_tool_add_topic(const char *topic, mqtt_deliver_mode_t mode)
{
    if (topic == RT_NULL || topic[0] == '\0')
    {
        return -RT_EINVAL;
    }

    /* 客户端未启动时自动启动；未连接时 sub_register 会预注册槽位，连上后自动订阅 */
    if (mqtt_tool_start() != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (sub_register(topic, mode) != RT_EOK)
    {
        return -RT_ERROR;
    }

    LOG_I("topic added: %s mode=%s", topic, mqtt_mode_to_string(mode));
    return RT_EOK;
}

/*
 * @brief 动态删除一个订阅话题
 * @note  异步生效：这里只清除订阅意图，实际的 UNSUBSCRIBE 报文与槽位释放
 *        由接收线程在下一个同步周期（<=200ms）完成
 * @param topic 话题过滤器
 * @return RT_EOK 成功，RT_ERROR 失败（未订阅 / 状态锁忙）
 */
rt_err_t mqtt_tool_remove_topic(const char *topic)
{
    mqtt_sub_entry_t *entry;
    rt_err_t ret = RT_EOK;

    if (topic == RT_NULL || topic[0] == '\0')
    {
        return -RT_EINVAL;
    }

    if (!lock_take())
    {
        return -RT_EBUSY;
    }

    entry = sub_find(topic);
    if (entry == RT_NULL)
    {
        ret = -RT_EINVAL;
    }
    else
    {
        entry->want = RT_FALSE;
    }

    lock_give();

    if (ret == RT_EOK)
    {
        LOG_I("topic removed (unsubscribe pending): %s", topic);
    }
    return ret;
}

/*
 * @brief 查询当前有效订阅数量
 * @return 已订阅（want=1）的话题数
 */
int mqtt_tool_topic_count(void)
{
    int i;
    int count = 0;

    if (!lock_take())
    {
        return 0;
    }
    for (i = 0; i < MQTT_TOOL_MAX_SUB_TOPICS; i++)
    {
        if (s_subs[i].topic[0] != '\0' && s_subs[i].want)
        {
            count++;
        }
    }
    lock_give();
    return count;
}

