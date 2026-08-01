#ifndef __AGENT_CONTEXT_H__
#define __AGENT_CONTEXT_H__

#include <rtthread.h>
#include "prompt.h"
#include "MessageHub.h"
#include "chat.h"
#include "utils.h"

#define LOG_TAG "Agent.context"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 角色类型枚举 */
typedef enum RoleType
{
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} RoleType;

typedef struct Context
{
    /* data */
    cJSON* message;

    rt_err_t (*build_system_prompt)(struct Context* self);
    rt_err_t (*append_user_message)(struct Context* self,Message_t message);
    rt_err_t (*append_assistant_message)(struct Context* self,ChatResponse_t resp);
    rt_err_t (*append_tool_message)(struct Context* self,const char* tool_call_id,const char* content);
}Context,*Context_t;


/* 函数声明 */
char* Role(RoleType type);
Context_t AgentContextCreate();

#endif /* __AGENT_CONTEXT_H__ */

