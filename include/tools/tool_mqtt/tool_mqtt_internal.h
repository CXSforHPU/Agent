#ifndef __AGENT_TOOL_MQTT_INTERNAL_H__
#define __AGENT_TOOL_MQTT_INTERNAL_H__

/*
 * MQTT 工具模块内部共享头
 * 仅供 src/tools/tool_mqtt 目录下的 .c 使用；对外接口见 tool_mqtt.h。
 *
 * 模块划分：
 *   tool_mqtt.c         引擎核心：全局状态、基础工具、paho 回调、缓冲与注入
 *   tool_mqtt_client.c  连接与订阅生命周期：订阅表/paho 槽位同步、启动停止
 *   tool_mqtt_route.c   接收线程与路由：门控放行、错峰、批量合并注入
 *   tool_mqtt_filter.c  本地过滤与阈值规则：咨询识别、越界判定、规则管理
 *   tool_mqtt_ops.c     业务操作：发布/订阅/退订/列表/收取（工具与 MSH 共用）
 *   tool_mqtt_tools.c   LLM 工具入口：4 个 tool_mqtt_* 执行函数
 *   tool_mqtt_cmd.c     MSH 调试命令：mqtt_tool ...
 */

#include "tool_mqtt.h"
#include "AgentRuntime.h"   /* agent_is_running / agent_is_busy / agent_get_message_hub */

/* ==========================================================================
 * 一、内部常量
 * ========================================================================== */

/* 接收线程等待消息的超时（毫秒）：同时作为订阅状态同步周期与 stop 响应周期 */
#define MQTT_TOOL_RX_WAIT_MS        200
/* 等待连接/订阅结果的轮询间隔（毫秒） */
#define MQTT_TOOL_POLL_STEP_MS      50
/* 放行原因文本长度上限 */
#define MQTT_TOOL_REASON_MAX_LEN    96
/* 注入文本缓冲上限（按最大批量估算，堆上分配） */
#define MQTT_TOOL_INJECT_TEXT_MAX   (PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX * \
                                     (MQTT_TOOL_TOPIC_MAX_LEN + MQTT_TOOL_PAYLOAD_MAX_LEN + 48) + 256)
/* 工具结果缓冲：小（发布/订阅）/ 中（订阅与规则列表）/ 大（批量收取）/ 超大（近期历史），均堆分配 */
#define MQTT_TOOL_RESULT_SMALL      256
#define MQTT_TOOL_RESULT_MID        768
#define MQTT_TOOL_RESULT_LARGE      (MQTT_TOOL_POLL_DEPTH * \
                                     (MQTT_TOOL_TOPIC_MAX_LEN + MQTT_TOOL_PAYLOAD_MAX_LEN + 48) + 512)
#define MQTT_TOOL_RESULT_XLARGE     (MQTT_TOOL_MAX_SUB_TOPICS * \
                                     (MQTT_TOOL_HISTORY_DEPTH * \
                                      (MQTT_TOOL_HISTORY_PAYLOAD_LEN + 48) + 384) + 512)
/* 状态锁最长等待时间（毫秒）：绝不无限等待，避免把 agent/MSH 卡死 */
#ifndef PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS
#define PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS    1000
#endif

/* ==========================================================================
 * 内部数据结构
 * ========================================================================== */

/* 最近消息历史中的一条（按订阅记录保存，读取不消费） */
typedef struct
{
    char payload[MQTT_TOOL_HISTORY_PAYLOAD_LEN];
    rt_tick_t tick;                      /* 到达时间（tick），用于换算"多久前" */
    rt_uint8_t truncated;
} mqtt_hist_item_t;

/* 一条订阅记录 */
typedef struct
{
    char topic[MQTT_TOOL_TOPIC_MAX_LEN]; /* 话题过滤器（支持 + / # 通配） */
    rt_bool_t want;                      /* 用户期望订阅：跨重连保持的订阅意图 */
    rt_bool_t registered;                /* 已占用 paho messageHandlers 槽位 */
    rt_bool_t subscribed;                /* 已向 broker 发出过 SUBSCRIBE */
    mqtt_deliver_mode_t mode;            /* 投递模式：filter / auto / poll */
    rt_uint32_t rx_count;                /* 该话题累计收到消息数 */
    rt_uint32_t inject_count;            /* 该话题累计交给 agent 分析的消息数 */
    rt_uint32_t suppressed_count;        /* filter 模式下被本地拦截（未打扰 agent）的消息数 */
    char last_payload[PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN]; /* 最近一条 payload（按需总结用） */
    rt_tick_t last_tick;                 /* 最近一条消息时间 */
    /* 最近 N 条消息（含被本地拦截的例行数据），供 mqtt_history 做近期总结/评估 */
    mqtt_hist_item_t hist[MQTT_TOOL_HISTORY_DEPTH];
    rt_uint8_t hist_count;               /* 已记录条数（<= HISTORY_DEPTH） */
    rt_uint8_t hist_head;                /* 最旧一条所在下标 */
} mqtt_sub_entry_t;

/* 一条阈值规则：命中（越界）时才把消息交给 agent 总结/告警 */
typedef struct
{
    rt_bool_t used;
    char topic[MQTT_TOOL_TOPIC_MAX_LEN];      /* 话题过滤器（支持通配） */
    char field[MQTT_TOOL_FIELD_MAX_LEN];      /* JSON 字段名；空表示对整条 payload 取值 */
    char op[4];                               /* ">", ">=", "<", "<=", "==", "!=" */
    double value;                             /* 阈值 */
    rt_bool_t triggered;                      /* 上次是否已越界（用于边沿判定） */
    rt_tick_t last_alert_tick;                /* 上次告警时间（冷却用） */
    rt_uint32_t alert_count;                  /* 该规则累计告警次数 */
} mqtt_rule_entry_t;

/* 接收队列/轮询缓冲中的一条消息 */
typedef struct
{
    char topic[MQTT_TOOL_TOPIC_MAX_LEN];
    char payload[MQTT_TOOL_PAYLOAD_MAX_LEN];
    rt_uint16_t payload_len;             /* 实际（截断后）长度 */
    rt_uint16_t raw_len;                 /* 原始长度，用于判断是否被截断 */
    rt_uint8_t truncated;
} mqtt_rx_item_t;

/* ==========================================================================
 * 三、共享状态（定义于 tool_mqtt.c）
 * ========================================================================== */

extern MQTTClient s_client;
extern rt_bool_t s_started;

extern mqtt_sub_entry_t s_subs[MQTT_TOOL_MAX_SUB_TOPICS];
extern mqtt_rule_entry_t s_rules[MQTT_TOOL_MAX_RULES];

extern mqtt_rx_item_t s_poll_ring[MQTT_TOOL_POLL_DEPTH];
extern int s_poll_head;
extern int s_poll_tail;
extern int s_poll_count;

extern rt_uint32_t s_rx_total;
extern rt_uint32_t s_rx_dropped;
extern rt_uint32_t s_poll_overwrites;
extern rt_uint32_t s_inject_total;
extern rt_uint32_t s_suppressed_total;
extern rt_uint32_t s_alert_total;

extern rt_mutex_t s_lock;
extern rt_mq_t s_rx_queue;
extern rt_thread_t s_rx_thread;
extern volatile rt_bool_t s_rx_running;
extern rt_sem_t s_rx_exit_sem;
extern rt_tick_t s_last_inject_tick;
extern rt_bool_t s_has_last;

/* ==========================================================================
 * 四、引擎核心（tool_mqtt.c）
 * ========================================================================== */

/* 状态锁：带超时获取，绝不无限等待；临界区内只允许纯内存操作 */
rt_bool_t lock_take(void);
void lock_give(void);
/* 幂等创建状态锁：客户端未启动时的只读操作（status/history/receive）也需要它 */
rt_bool_t mqtt_tool_lock_init(void);

rt_int32_t ticks_to_ms(rt_tick_t ticks);
rt_bool_t has_wildcard(const char *str);
void buf_append(char *buf, rt_size_t size, rt_size_t *used, const char *fmt, ...);

mqtt_deliver_mode_t mqtt_mode_from_string(const char *mode);
const char *mqtt_mode_to_string(mqtt_deliver_mode_t mode);
rt_bool_t topic_filter_match(const char *filter, const char *topic);

mqtt_sub_entry_t *sub_find(const char *topic);
mqtt_sub_entry_t *sub_match(const char *topic);
mqtt_sub_entry_t *sub_find_free(void);

/*
 * @brief 把 JSON 节点转成数值（兼容数值字符串与布尔），供规则判定与历史统计共用
 * @param node 节点
 * @param out  输出数值
 * @return RT_TRUE 转换成功
 */
rt_bool_t mqtt_json_node_number(cJSON *node, double *out);
/* 把一条消息追加进订阅记录的近期历史环形缓冲（调用者需持有 s_lock） */
void mqtt_hist_push(mqtt_sub_entry_t *entry, const mqtt_rx_item_t *item);

/* 轮询缓冲（poll 模式与 filter 模式的滚动窗口） */
void poll_push(const mqtt_rx_item_t *item);
int poll_clear(void);

/* 把一段文本作为用户消息注入 agent（等待 LLM 总结/告警） */
rt_err_t mqtt_inject_text(const char *text);

/* ==========================================================================
 * 五、连接与订阅生命周期（tool_mqtt_client.c）
 * ========================================================================== */

void subs_sync_registered(void);
void subs_sync_with_client(void);
rt_err_t sub_register(const char *topic, mqtt_deliver_mode_t mode);
rt_bool_t mqtt_wait_connected(rt_int32_t timeout_ms);

/* ==========================================================================
 * 六、本地过滤与阈值规则（tool_mqtt_filter.c）
 * ========================================================================== */

/*
 * @brief 判断报文是否像「需要咨询」（问句/求助/异常类关键词）
 * @param payload 报文内容
 * @return RT_TRUE 需要交给 agent
 */
rt_bool_t payload_looks_like_ask(const char *payload);

/*
 * @brief 阈值规则判定（含边沿触发与告警冷却）
 * @param item   消息
 * @param reason 命中原因（输出，可为 NULL）
 * @param size   原因缓冲大小
 * @return RT_TRUE 需要交给 agent
 */
rt_bool_t rule_gate(const mqtt_rx_item_t *item, char *reason, rt_size_t size);

/*
 * @brief 设置/清除阈值规则
 * @param action "set" 设置，其它值清除
 * @param topic  话题过滤器（clear 时 "*" 表示全部）
 * @param field  JSON 字段名（可为 NULL：整条 payload 视为数值）
 * @param op     比较符
 * @param value  阈值
 * @param out    结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_do_rule(const char *action, const char *topic, const char *field,
                      const char *op, double value, char *out, rt_size_t out_size);

/* ==========================================================================
 * 七、业务操作（tool_mqtt_ops.c）—— LLM 工具与 MSH 命令共用
 * ========================================================================== */

/*
 * @brief 连接 broker（连接前统一检查：未启动则启动，未连上则等待）
 * @param timeout_ms 等待连接的超时（毫秒），<=0 时使用 PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS
 * @param out        结果文本
 * @param out_size   结果缓冲大小
 * @return RT_EOK 已连接，RT_ERROR 失败
 */
rt_err_t mqtt_do_connect(rt_int32_t timeout_ms, char *out, rt_size_t out_size);

/*
 * @brief 断开连接并释放工作线程/接收线程/队列（订阅意图保留，下次连接自动重订阅）
 * @param out      结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功
 */
rt_err_t mqtt_do_disconnect(char *out, rt_size_t out_size);

rt_err_t mqtt_do_publish(const char *topic, const char *message, char *out, rt_size_t out_size);
rt_err_t mqtt_do_subscribe(const char *topic, mqtt_deliver_mode_t mode, char *out, rt_size_t out_size);
rt_err_t mqtt_do_unsubscribe(const char *topic, char *out, rt_size_t out_size);
void mqtt_do_list(char *out, rt_size_t out_size);
int mqtt_do_receive(int max, const char *topic_filter, char *out, rt_size_t out_size);

/*
 * @brief 汇总某话题（或全部话题）的近期情况：最近消息（含 payload）+ 数值字段统计 + 计数
 * @note  只读，不消费历史；数据来源为每个订阅记录的近期环形历史
 * @param topic_filter 话题过滤器（NULL/空表示全部）
 * @param max          每个话题最多列出多少条近期消息
 * @param out          结果文本
 * @param out_size     结果缓冲大小
 * @return 汇总到的话题数
 */
int mqtt_do_history(const char *topic_filter, int max, char *out, rt_size_t out_size);

/* ==========================================================================
 * 八、paho 回调与接收线程入口
 * ========================================================================== */

/* paho 事件/消息回调（定义于 tool_mqtt.c，由 tool_mqtt_client.c 注册到客户端） */
void mqtt_connect_callback(MQTTClient *c);
void mqtt_online_callback(MQTTClient *c);
void mqtt_offline_callback(MQTTClient *c);
void mqtt_sub_callback(MQTTClient *c, MessageData *msg_data);

/* 接收线程入口（定义于 tool_mqtt_route.c，由 tool_mqtt_client.c 创建） */
void mqtt_rx_thread(void *param);

#endif /* __AGENT_TOOL_MQTT_INTERNAL_H__ */
