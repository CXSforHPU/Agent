#ifndef __AGENT_UTILS_AGENT_REPORT_H__
#define __AGENT_UTILS_AGENT_REPORT_H__

#include <rtthread.h>

/*
 * 自发上报通道：agent 自己产生的结论（MQTT 告警、传感器越限等）
 * 推给控制台，而不是等谁来问。
 *
 * 控制台通过 A2A 的 POST /tasks/pushNotificationConfig 下发回调地址，
 * 板端存进 SD 卡；此后每条自发结论都会 POST 到那个地址。
 */

/* 单条上报的文本上限，超出部分截断 */
#define AGENT_REPORT_TEXT_MAX   512
/* 回调地址长度上限 */
#define AGENT_REPORT_URL_MAX    192
/* 回调标识长度上限 */
#define AGENT_REPORT_ID_MAX     64
/* 待上报队列深度，满了丢最旧的一条 */
#define AGENT_REPORT_QUEUE_SIZE 4

/*
 * 上报配置文件路径，控制台下发的回调地址存在这里，重启后仍然有效
 */
#ifndef PKG_AGENT_A2A_PUSH_PATH
#define PKG_AGENT_A2A_PUSH_PATH   "/agent_push.json"
#endif

/*
 * @brief 初始化上报模块：读回上次下发的回调地址
 */
void agent_report_init(void);

/*
 * @brief 释放上报模块资源
 */
void agent_report_deinit(void);

/*
 * @brief 入队一条自发结论
 * @param text 结论文本（非空）
 * @return RT_EOK 已入队，RT_ERROR 参数非法或队列已满
 * @note  只入队，真正发出去由上报线程做；没有配置回调地址时事件留在队列里
 */
rt_err_t agent_report_push(const char *text);

/*
 * @brief 设置上报回调地址
 * @param url 回调地址，NULL 或空串表示关闭上报
 * @param id  回调标识，可为 NULL
 * @return RT_EOK 成功
 * @note  地址变了才写 SD 卡，重复下发同样的地址不会反复擦写
 */
rt_err_t agent_report_set_callback(const char *url, const char *id);

/*
 * @brief 取当前回调地址
 * @return 地址字符串，未配置时返回空串
 */
const char *agent_report_callback(void);

/*
 * @brief 取当前回调标识
 * @return 标识字符串，未配置时返回空串
 */
const char *agent_report_callback_id(void);

/*
 * @brief 查询是否配了回调地址
 */
rt_bool_t agent_report_ready(void);

#endif /* __AGENT_UTILS_AGENT_REPORT_H__ */
