#ifndef __AGENT_CHANNELS_H__
#define __AGENT_CHANNELS_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"
#include "AgentChannel.h"

/*
 * 通道选择（单点宏，仅此处出现 PKG_AGENT_*_CHANNEL 判断）：
 * AgentLoop 通过 g_channel_ops（AgentChannelOps 函数指针）统一分发 init/reset，
 * 调用点不再出现通道相关 #ifdef。
 */
#ifdef PKG_AGENT_CLI_CHANNEL
    #include "CLI.h"
    #define AGENT_CHANNEL_OPS  (&agent_cli_ops)
#elif defined(PKG_AGENT_WEBNET_CHANNEL)
    #include "AgentWebChannel.h"
    #define AGENT_CHANNEL_OPS  (&agent_webnet_ops)
#elif defined(PKG_AGENT_DEBUG_CHANNEL)
    #include "AgentDebugChannel.h"
    #define AGENT_CHANNEL_OPS  (&agent_debug_ops)
#else
    /* 未配置通道：AgentLoop 中对 NULL 的 ops 安全跳过 */
    #define AGENT_CHANNEL_OPS  ((AgentChannelOps *)RT_NULL)
#endif

#endif /* __AGENT_CHANNELS_H__ */
