#ifndef __AGENT_RUNTIME_H__
#define __AGENT_RUNTIME_H__

#include <rtthread.h>
#include "MessageHub.h"

/*
 * @brief 获取当前 agent 的消息中心句柄
 * @note  供需要主动向 agent 注入消息的外部模块调用（例如 MQTT 订阅回调、
 *        设备事件上报等）。agent 未运行时返回 RT_NULL，调用方需自行处理
 *        （缓存待注入消息或丢弃），不得在 RT_NULL 时解引用。
 * @return MessageHub_t 句柄，agent 未运行返回 RT_NULL
 */
MessageHub_t agent_get_message_hub(void);

/*
 * @brief 查询 agent 主循环是否正在运行
 * @return RT_TRUE 运行中，RT_FALSE 未运行
 */
rt_bool_t agent_is_running(void);

/*
 * @brief 查询 agent 是否正在处理一轮对话（含 LLM 请求与工具执行）
 * @note  供需要主动注入消息的模块做「错峰」，例如 MQTT 接收线程只在 agent
 *        空闲时投递消息，避免连续请求把 API/网络打爆（对端重置会表现为
 *        mbedtls NET_RECV_FAILED）。主循环阻塞等待新消息期间返回 RT_FALSE。
 * @return RT_TRUE 忙，RT_FALSE 空闲
 */
rt_bool_t agent_is_busy(void);

#endif /* __AGENT_RUNTIME_H__ */
