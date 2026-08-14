#ifndef __AGENT_CHANNELS_H__
#define __AGENT_CHANNELS_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"


#ifdef PKG_AGENT_CLI_CHANNEL
    #include "CLI.h"
    #define AGENT_CHANNEL_IMPL  AgentCLIChannel
#elif defined(PKG_AGENT_WEBNET_CHANNEL)
    #include "AgentWebChannel.h"
    #define AGENT_CHANNEL_IMPL  webnet_agent_mode
#elif defined(PKG_AGENT_DEBUG_CHANNEL)
    #include "AgentDebugChannel.h"
    #define AGENT_CHANNEL_IMPL AgentDebugChannel
#else
#define AGENT_CHANNEL_IMPL  RT_NULL
#endif


#endif /* __AGENT_CHANNELS_H__ */ 