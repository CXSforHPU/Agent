#ifndef __AGENT_WEBNET_H__
#define __AGENT_WEBNET_H__
#include <rtthread.h>
#include <rtthread.h>
#include <webnet.h>
#include "MessageHub.h"
#include "context.h"
#include <wn_module.h>

#include "AgentConfig.h"
#include <string.h>

#define LOG_TAG "Agent.channel.webnet"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>




void webnet_agent_mode(MessageHub_t hub, Context_t ctx);

#endif /* __AGENT_WEBNET_H__ */
