#ifndef __DEBUG_CHANNEL_H__
#define __DEBUG_CHANNEL_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"

#define LOG_TAG "Agent.Channels.DebugChannel"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>


int AgentDebugChannel(MessageHub_t message_hub, Context_t context);




#endif /* __DEBUG_CHANNEL_H */