#ifndef __DEBUG_CHANNEL_H__
#define __DEBUG_CHANNEL_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"
#include "AgentChannel.h"

#define LOG_TAG "Agent.Channels.DebugChannel"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * @brief Debug 通道初始化（绑定 message_hub / context）
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return 0 成功
 */
int agent_debug_channel(MessageHub_t message_hub, Context_t context);

/*
 * @brief Debug 通道清理（置空缓存的 hub/context 指针，幂等）
 * @note 作为 AgentChannelOps.reset 实现，用于清理流程
 */
void agent_debug_reset(void);

/* Debug 通道 ops 实例（init=agent_debug_channel, reset=agent_debug_reset） */
extern AgentChannelOps agent_debug_ops;

#endif /* __DEBUG_CHANNEL_H */