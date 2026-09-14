#ifndef __AGENT_TOOL_MQTT_H__
#define __AGENT_TOOL_MQTT_H__

#include "rtconfig.h"

#include "tool_base.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <rtthread.h>
#include "paho_mqtt.h"

/* ==========================================================================
 * 一、连接参数（menuconfig: PKG_AGENT_TOOL_MQTT_*）
 *     全部使用 #ifndef 兜底，缺少 menuconfig 选项时也能编译运行
 * ========================================================================== */
#ifndef PKG_AGENT_TOOL_MQTT_BROKER_URL
#define PKG_AGENT_TOOL_MQTT_BROKER_URL      "tcp://127.0.0.1:1883"
#endif
#ifndef PKG_AGENT_TOOL_MQTT_PUB_TOPIC
#define PKG_AGENT_TOOL_MQTT_PUB_TOPIC       "agent/pub"
#endif
#ifndef PKG_AGENT_TOOL_MQTT_SUB_TOPIC
#define PKG_AGENT_TOOL_MQTT_SUB_TOPIC       "agent/sub"
#endif
#ifndef PKG_AGENT_TOOL_MQTT_CLIENT_ID
#define PKG_AGENT_TOOL_MQTT_CLIENT_ID       "rt-thread-agent"
#endif
#ifndef PKG_AGENT_TOOL_MQTT_USERNAME
#define PKG_AGENT_TOOL_MQTT_USERNAME        ""
#endif
#ifndef PKG_AGENT_TOOL_MQTT_PASSWORD
#define PKG_AGENT_TOOL_MQTT_PASSWORD        ""
#endif
#ifndef PKG_AGENT_TOOL_MQTT_WILLMSG
#define PKG_AGENT_TOOL_MQTT_WILLMSG         "Goodbye from RT-Thread Agent"
#endif
#ifndef PKG_AGENT_TOOL_MQTT_BUF_SIZE
#define PKG_AGENT_TOOL_MQTT_BUF_SIZE        1024
#endif


#define MQTT_SERVER_URI         PKG_AGENT_TOOL_MQTT_BROKER_URL
#define MQTT_CLIENTID           PKG_AGENT_TOOL_MQTT_CLIENT_ID
#define MQTT_USERNAME           PKG_AGENT_TOOL_MQTT_USERNAME
#define MQTT_PASSWORD           PKG_AGENT_TOOL_MQTT_PASSWORD
#define MQTT_SUBTOPIC           PKG_AGENT_TOOL_MQTT_SUB_TOPIC
#define MQTT_PUBTOPIC           PKG_AGENT_TOOL_MQTT_PUB_TOPIC
#define MQTT_WILLMSG            PKG_AGENT_TOOL_MQTT_WILLMSG
#define MQTT_PUB_SUB_BUF_SIZE   PKG_AGENT_TOOL_MQTT_BUF_SIZE

/*
 * paho-mqtt（pipe 模式）的收发接口仅支持 QOS1：
 * paho_mqtt_publish() / paho_mqtt_subscribe() 内部会拒绝非 QOS1，
 * 因此本工具统一使用 QOS1，不跟随 PKG_AGENT_TOOL_MQTT_QOS。
 */
#define MQTT_TOOL_QOS           QOS1

/* ==========================================================================
 * 二、工具自身可调参数（可在 rtconfig.h 中覆盖，或用 -D 传入）
 * ========================================================================== */

/* 最多同时订阅的话题数（受 paho MAX_MESSAGE_HANDLERS 限制，自动收敛） */
#ifndef PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS
#define PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS  4
#endif
#if (PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS > MAX_MESSAGE_HANDLERS)
#warning "PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS clamped to paho MAX_MESSAGE_HANDLERS; \
raise PKG_PAHOMQTT_SUBSCRIBE_HANDLERS in the paho menuconfig to subscribe more topics"
#undef  PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS
#define PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS  MAX_MESSAGE_HANDLERS
#endif
#if (PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS < 1)
#undef  PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS
#define PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS  1
#endif

/* ==========================================================================
 * 二·补、paho 侧依赖检查（编译期给出可执行的提示，避免运行期才发现）
 * ========================================================================== */

/* 本工具依赖 paho 的 pipe 模式：UDP 模式不提供 paho_mqtt_publish/subscribe 接口 */
#if !defined(PAHOMQTT_PIPE_MODE) && !defined(PAHOMQTT_UDP_MODE)
#warning "PAHOMQTT_PIPE_MODE not defined: enable paho-mqtt and select PAHOMQTT_PIPE_MODE \
(Agent MQTT tool needs paho_mqtt_start/publish/subscribe from paho_mqtt_pipe.c)"
#elif !defined(PAHOMQTT_PIPE_MODE) && defined(PAHOMQTT_UDP_MODE)
#error "Agent MQTT tool requires paho PIPE mode: switch PAHOMQTT_UDP_MODE off and \
PAHOMQTT_PIPE_MODE on (UDP mode provides no publish/subscribe API)"
#endif

/*
 * paho 工作线程栈：启动时会 open("/dev/MQTTn") 创建发布管道，该路径经 DFS 解析
 * （dfs_file_realpath 会 rt_malloc(DFS_PATH_MAX*3+3)，DFS_PATH_MAX=4096 时即 12KB）。
 * 栈不足会溢出并踩坏堆元数据（线程栈本身分配在堆上），表现为启动后整机卡死。
 */
#ifdef RT_PKG_MQTT_THREAD_STACK_SIZE
#if (RT_PKG_MQTT_THREAD_STACK_SIZE < 8192)
#warning "RT_PKG_MQTT_THREAD_STACK_SIZE < 8192: paho worker may overflow its stack \
inside DFS open() and hang the system; set it to 8192 or more in the paho menuconfig"
#endif
#else
#warning "RT_PKG_MQTT_THREAD_STACK_SIZE not defined: make sure the paho worker stack is \
at least 8192 bytes (see PKG_PAHOMQTT thread stack option)"
#endif

/* 话题字符串缓冲区长度（含 '\0'） */
#ifndef PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN
#define PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN   64
#endif

/* 接收缓冲的单条 payload 上限（超出截断） */
#ifndef PKG_AGENT_TOOL_MQTT_PAYLOAD_MAX_LEN
#define PKG_AGENT_TOOL_MQTT_PAYLOAD_MAX_LEN 256
#endif

/* 单次发布 payload 上限（超出报错；还需满足 paho 的 buf/pipe 限制） */
#ifndef PKG_AGENT_TOOL_MQTT_MAX_PUB_LEN
#define PKG_AGENT_TOOL_MQTT_MAX_PUB_LEN     256
#endif

/*
 * paho（pipe 模式）把待发布报文写入 rt_pipe 后由工作线程取出发送，
 * 整条报文必须能一次性写入 pipe，因此发布长度还受 pipe 缓冲限制
 */
#ifndef PKG_AGENT_TOOL_MQTT_PIPE_BUF_SIZE
#ifdef RT_USING_POSIX_PIPE_SIZE
#define PKG_AGENT_TOOL_MQTT_PIPE_BUF_SIZE   RT_USING_POSIX_PIPE_SIZE
#else
#define PKG_AGENT_TOOL_MQTT_PIPE_BUF_SIZE   512
#endif
#endif

/* 接收队列深度：MQTT 回调 -> 接收线程，队列满时丢弃最旧消息 */
#ifndef PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH
#define PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH  16
#endif

/* 手动（poll）模式环形缓冲深度（同时作为 filter 模式"最近消息"滚动窗口） */
#ifndef PKG_AGENT_TOOL_MQTT_POLL_DEPTH
#define PKG_AGENT_TOOL_MQTT_POLL_DEPTH      8
#endif

/* 阈值/告警规则条数上限 */
#ifndef PKG_AGENT_TOOL_MQTT_MAX_RULES
#define PKG_AGENT_TOOL_MQTT_MAX_RULES       4
#endif

/* 规则中 JSON 字段名长度上限 */
#ifndef PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN
#define PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN   24
#endif

/* 订阅记录的"最近一条 payload"缓存长度（供 status/list 查看，便于按需总结） */
#ifndef PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN
#define PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN 96
#endif

/*
 * 每个订阅话题保留的"最近消息历史"条数与单条 payload 长度：
 * 供 mqtt_history 工具做「近期情况总结/评估」——filter 模式下被本地拦截的例行数据
 * 也完整保留在这里（含原始 payload），Agent 需要时可随时读取，且读取不消费历史。
 */
#ifndef PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH
#define PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH    8
#endif
#ifndef PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN
#define PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN 128
#endif

/* 同一规则的告警冷却时间（毫秒）：越界持续期间不会每条消息都打扰 agent */
#ifndef PKG_AGENT_TOOL_MQTT_ALERT_COOLDOWN_MS
#define PKG_AGENT_TOOL_MQTT_ALERT_COOLDOWN_MS   60000
#endif

/* 两次注入 agent 之间的最小间隔（毫秒）：避免把 LLM/网络打爆（对端会因请求过密而重置连接） */
#ifndef PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS
#define PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS  1000
#endif

/* 单次注入 agent 最多合并的消息条数 */
#ifndef PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX
#define PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX        5
#endif

/* agent 忙时最多等待其空闲多久（毫秒）再投递，超时则转入 poll 缓冲不丢消息 */
#ifndef PKG_AGENT_TOOL_MQTT_BUSY_WAIT_MS
#define PKG_AGENT_TOOL_MQTT_BUSY_WAIT_MS            180000
#endif

/*
 * 默认订阅话题（PKG_AGENT_TOOL_MQTT_SUB_TOPIC）的投递模式：filter / auto / poll
 * menuconfig 用 choice 单选（PKG_AGENT_TOOL_MQTT_DEFAULT_MODE_FILTER/AUTO/POLL），
 * 这里统一映射成字符串；也可直接定义 PKG_AGENT_TOOL_MQTT_DEFAULT_MODE 覆盖。
 */
#ifndef PKG_AGENT_TOOL_MQTT_DEFAULT_MODE
#if defined(PKG_AGENT_TOOL_MQTT_DEFAULT_MODE_AUTO)
#define PKG_AGENT_TOOL_MQTT_DEFAULT_MODE            "auto"
#elif defined(PKG_AGENT_TOOL_MQTT_DEFAULT_MODE_POLL)
#define PKG_AGENT_TOOL_MQTT_DEFAULT_MODE            "poll"
#else
#define PKG_AGENT_TOOL_MQTT_DEFAULT_MODE            "filter"
#endif
#endif

/* 等待 MQTT 连接/订阅生效的超时（毫秒） */
#ifndef PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS
#define PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS         3000
#endif
#ifndef PKG_AGENT_TOOL_MQTT_SUB_WAIT_MS
#define PKG_AGENT_TOOL_MQTT_SUB_WAIT_MS             3000
#endif

/* keepalive 间隔（秒） */
#ifndef PKG_AGENT_TOOL_MQTT_KEEPALIVE_SEC
#define PKG_AGENT_TOOL_MQTT_KEEPALIVE_SEC           60
#endif

/* 接收线程栈大小 / 优先级 */
#ifndef PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK
#define PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK         3072
#endif
#ifndef PKG_AGENT_TOOL_MQTT_RX_THREAD_PRIO
#define PKG_AGENT_TOOL_MQTT_RX_THREAD_PRIO          15
#endif

/*
 * 客户端启动时是否自动订阅 PKG_AGENT_TOOL_MQTT_SUB_TOPIC
 * 注意：Kconfig 的 bool 选项选中后是「空展开」（rtconfig.h 里是 #define X 而没有值），
 *       不能用 #if (X) 判断，这里统一归一化成 0/1 供代码使用。
 */
#ifdef PKG_AGENT_TOOL_MQTT_AUTO_SUB_DEFAULT
#define MQTT_TOOL_AUTO_SUB_DEFAULT                  1
#else
#define MQTT_TOOL_AUTO_SUB_DEFAULT                  0
#endif

/* 注入 agent 的消息前缀（printf 格式：话题、内容、放行原因） */
#ifndef PKG_AGENT_TOOL_MQTT_INJECT_FMT
#define PKG_AGENT_TOOL_MQTT_INJECT_FMT \
    "[MQTT message forwarded for analysis]\ntopic: %s\npayload: %s\nforward reason: %s\n" \
    "Reply with a short summary or alert for the user. Keep it brief and do not restate routine data."
#endif

/* ==========================================================================
 * 二·补·参数归一化（防止 menuconfig 传入 0 或过小值造成除零 / 数组越界）
 *       Kconfig 侧也加了 range 限制，这里是最后一道防线
 * ========================================================================== */
#if (PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH < 1)
#undef  PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH
#define PKG_AGENT_TOOL_MQTT_RX_QUEUE_DEPTH  1
#endif
#if (PKG_AGENT_TOOL_MQTT_POLL_DEPTH < 1)          /* 取模运算的除数，不能为 0 */
#undef  PKG_AGENT_TOOL_MQTT_POLL_DEPTH
#define PKG_AGENT_TOOL_MQTT_POLL_DEPTH      1
#endif
#if (PKG_AGENT_TOOL_MQTT_MAX_RULES < 1)
#undef  PKG_AGENT_TOOL_MQTT_MAX_RULES
#define PKG_AGENT_TOOL_MQTT_MAX_RULES       1
#endif
#if (PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH < 1)       /* 取模运算的除数，不能为 0 */
#undef  PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH
#define PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH   1
#endif
#if (PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX < 1)
#undef  PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX
#define PKG_AGENT_TOOL_MQTT_INJECT_BATCH_MAX 1
#endif
#if (PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN < 16)
#undef  PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN
#define PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN   16
#endif
#if (PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN < 8)
#undef  PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN
#define PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN   8
#endif
#if (PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN < 16)
#undef  PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN
#define PKG_AGENT_TOOL_MQTT_LAST_PAYLOAD_LEN 16
#endif
#if (PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN < 16)
#undef  PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN
#define PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN 16
#endif
#if (PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS < 0)
#undef  PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS
#define PKG_AGENT_TOOL_MQTT_INJECT_MIN_INTERVAL_MS  0
#endif
#if (PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS < 1)
#undef  PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS
#define PKG_AGENT_TOOL_MQTT_LOCK_WAIT_MS    1
#endif

/* ==========================================================================
 * 三、投递模式
 * ========================================================================== */

/*
 * filter（默认）：只有「需要咨询」（报文像提问）或「命中阈值规则」时才交给 agent 总结回复，
 *                 其余消息只统计并从滚动窗口保留，需要时用 mqtt_receive 按需取用
 * auto          ：该话题每条消息都交给 agent 分析（旧行为，适用于命令/事件类话题）
 * poll          ：都不自动交给 agent，全部由 mqtt_receive 取用
 */
typedef enum
{
    MQTT_DELIVER_FILTER = 0,
    MQTT_DELIVER_AUTO,
    MQTT_DELIVER_POLL
} mqtt_deliver_mode_t;

#define MQTT_TOOL_MAX_SUB_TOPICS    PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS
#define MQTT_TOOL_TOPIC_MAX_LEN     PKG_AGENT_TOOL_MQTT_TOPIC_MAX_LEN
#define MQTT_TOOL_PAYLOAD_MAX_LEN   PKG_AGENT_TOOL_MQTT_PAYLOAD_MAX_LEN
#define MQTT_TOOL_POLL_DEPTH        PKG_AGENT_TOOL_MQTT_POLL_DEPTH
#define MQTT_TOOL_MAX_RULES         PKG_AGENT_TOOL_MQTT_MAX_RULES
#define MQTT_TOOL_FIELD_MAX_LEN     PKG_AGENT_TOOL_MQTT_FIELD_MAX_LEN
#define MQTT_TOOL_HISTORY_DEPTH     PKG_AGENT_TOOL_MQTT_HISTORY_DEPTH
#define MQTT_TOOL_HISTORY_PAYLOAD_LEN PKG_AGENT_TOOL_MQTT_HISTORY_PAYLOAD_LEN

/* ==========================================================================
 * 四、给 LLM 使用的工具执行函数
 * ========================================================================== */

/*
 * @brief MQTT 发布工具：向指定话题发布消息（下发命令）
 * @param args_obj 参数 JSON 对象 {topic?: string, message: string}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_publish(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 订阅工具：订阅/退订话题、切换投递模式、查询订阅列表
 * @param args_obj 参数 JSON 对象 {action: string, topic?: string, mode?: string}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_subscribe(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 阈值规则工具：设置/清除/查看"越界才打扰 agent"的规则
 * @param args_obj 参数 JSON 对象 {action: string, topic?: string, field?: string,
 *                                 op?: string, value?: number}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_rule(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 收取工具：取出 poll 模式缓冲的消息（取出即从缓冲移除）
 * @param args_obj 参数 JSON 对象 {max?: number, topic?: string}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_receive(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 近期情况工具：汇总某话题最近若干条消息（含原始 payload）+ 数值字段统计
 * @note  只读、不消费历史，适合回答"最近情况如何/帮我评估一下"这类问题
 * @param args_obj 参数 JSON 对象 {topic?: string, max?: number}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_history(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 连接工具：确保客户端已启动并连上 broker
 * @note  发布/订阅内部也会自动做同样的检查；显式调用可用来看清连接状态与失败原因
 * @param args_obj 参数 JSON 对象 {timeout_ms?: number}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_connect(cJSON *args_obj, AgentToolNode_t node);

/*
 * @brief MQTT 断开工具：断开连接并释放工作线程/接收线程/队列
 * @note  订阅表会保留（订阅意图不清除），下次连接后自动重新订阅
 * @param args_obj 参数 JSON 对象（可为空）
 * @param node     工具链表节点（用于返回结果）
 */
void tool_mqtt_disconnect(cJSON *args_obj, AgentToolNode_t node);

/* ==========================================================================
 * 五、应用层动态增删订阅话题（与 LLM 工具 / MSH 命令行为一致）
 * ========================================================================== */

/*
 * @brief 动态增加（或更新）一个订阅话题
 * @note  客户端未启动时会自动启动；未连接时先登记，连接建立后自动订阅
 * @param topic 话题过滤器（支持 + / # 通配；上限见 PKG_AGENT_TOOL_MQTT_MAX_SUB_TOPICS）
 * @param mode  投递模式（filter / auto / poll）
 * @return RT_EOK 成功，RT_ERROR 失败（参数非法 / 订阅表已满 / 状态锁忙）
 */
rt_err_t mqtt_tool_add_topic(const char *topic, mqtt_deliver_mode_t mode);

/*
 * @brief 动态删除一个订阅话题（异步：由接收线程完成 UNSUBSCRIBE 与槽位释放）
 * @param topic 话题过滤器
 * @return RT_EOK 成功，RT_ERROR 失败（未订阅 / 状态锁忙）
 */
rt_err_t mqtt_tool_remove_topic(const char *topic);

/*
 * @brief 查询当前有效订阅数量
 * @return 已订阅（want=1）的话题数
 */
int mqtt_tool_topic_count(void);

/* ==========================================================================
 * 五、生命周期接口（工具内部按需自动调用，也可从应用层显式控制）
 * ========================================================================== */

/*
 * @brief 启动 MQTT 客户端（幂等）：创建接收队列/接收线程并启动 paho 工作线程
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_tool_start(void);

/*
 * @brief 停止 MQTT 客户端并释放接收队列/线程（幂等）
 */
void mqtt_tool_stop(void);

/*
 * @brief MQTT 客户端是否已启动
 * @return RT_TRUE 已启动
 */
rt_bool_t mqtt_tool_is_started(void);

/*
 * @brief 查询当前连接状态（用于结果文本与排查）
 * @return "stopped" / "connecting" / "connected"
 */
const char *mqtt_tool_conn_state(void);

/*
 * @brief 确保客户端已启动并已连接（发布/订阅前的统一前置检查）
 * @note  未启动时自动启动；未连接时等待最多 PKG_AGENT_TOOL_MQTT_CONNECT_WAIT_MS；
 *        仍失败则给出可执行的错误原因（paho 工作线程会在后台每 5s 自动重连）
 * @param out      结果文本（可为 NULL，用于把检查过程写进工具结果）
 * @param out_size 结果缓冲大小
 * @return RT_TRUE 已连接
 */
rt_bool_t mqtt_tool_ensure_connected(char *out, rt_size_t out_size);

#endif // __AGENT_TOOL_MQTT_H__
