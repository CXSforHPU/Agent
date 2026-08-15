#ifndef __DEBUG_CHANNEL_H__
#define __DEBUG_CHANNEL_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"

#define LOG_TAG "Agent.Channels.DebugChannel"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * @brief Debug 通道初始化
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return 0 成功
 */
int agent_debug_channel(MessageHub_t message_hub, Context_t context);

#endif /* __DEBUG_CHANNEL_H */