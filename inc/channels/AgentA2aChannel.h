#ifndef __AGENT_A2A_CHANNEL_H__
#define __AGENT_A2A_CHANNEL_H__

#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"
#include "AgentChannel.h"

/*
 * A2A 通道：跑在 WebNet 之上，把 CGI 根切到 "/"，
 * 让 /.well-known/agent.json、/tasks/send 这些带点号和多级的路径能被匹配到。
 * 端口用 WebNet 的 WEBNET_PORT。
 */
void a2a_agent_channel_init(MessageHub_t hub, Context_t ctx);

/* A2A 通道 ops 实例（init=内部包装 a2a_agent_channel_init, reset=agent_a2a_reset） */
extern AgentChannelOps agent_a2a_ops;

#endif /* __AGENT_A2A_CHANNEL_H__ */
