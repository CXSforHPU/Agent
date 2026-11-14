#ifndef AGENT_A2A_CHANNEL_H
#define AGENT_A2A_CHANNEL_H
#include <rtthread.h>
#include "MessageHub.h"
#include "context.h"
#include "AgentChannel.h"
/* Authenticated outbound WebSocket client; exclusive mailbox consumer. */
extern AgentChannelOps agent_a2a_ops;
#endif
