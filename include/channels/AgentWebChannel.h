#ifndef __AGENT_WEBNET_H__
#define __AGENT_WEBNET_H__
#include <rtthread.h>
#include <webnet.h>
#include "MessageHub.h"
#include "context.h"
#include <wn_module.h>

#include "AgentConfig.h"
#include <string.h>
#include "AgentChannel.h"

#define LOG_TAG "Agent.channel.webnet"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * @brief WebNet 通道初始化（注册 CGI 处理器并启动 webnet）
 * @param hub 消息中心句柄
 * @param ctx 上下文管理器句柄
 */
void webnet_agent_mode(MessageHub_t hub, Context_t ctx);

/*
 * @brief WebNet 通道清理（置空缓存的 hub/context 指针，幂等）
 * @note 作为 AgentChannelOps.reset 实现，用于清理流程
 */
void agent_webnet_reset(void);

/* WebNet 通道 ops 实例（init=内部包装 webnet_agent_mode, reset=agent_webnet_reset） */
extern AgentChannelOps agent_webnet_ops;

#endif /* __AGENT_WEBNET_H__ */