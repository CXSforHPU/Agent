#ifndef __AGENT_CHAT_H__
#define __AGENT_CHAT_H__

#include <rtthread.h>
#include "utils.h"
#include "webclient.h"
#include "AgentConfig.h"

#define LOG_TAG "Agent.chat"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

typedef struct ChatResponse
{
    char* reasoning;
    char* context;
    cJSON* tool_call;    //[list[Dict]]
} ChatResponse,*ChatResponse_t;

ChatResponse_t chat(
    cJSON* messages,
    cJSON* tools,
    int max_tokens,
    void (*on_reasoning)(const char* text),
    void (*on_tool_call)(const char* text),
    void (*on_context)(const char* text)
);
void chat_response_free(ChatResponse_t resp);
#endif /* __AGENT_CHAT_H__ */
