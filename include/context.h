#ifndef __AGENT_CONTEXT_H__
#define __AGENT_CONTEXT_H__
#include "rtconfig.h"
#include <rtthread.h>
#include "prompt.h"
#include "MessageHub.h"
#include "chat.h"
#include "utils.h"

/* 角色类型枚举 */
typedef enum RoleType
{
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} RoleType;

/* 上下文管理器结构 */
typedef struct Context
{
    cJSON *message;  /* 消息列表 cJSON 数组 */

    rt_err_t (*build_system_prompt)(struct Context *self);
    rt_err_t (*append_user_message)(struct Context *self, Messages_t messages);
    rt_err_t (*append_assistant_message)(struct Context *self, ChatResponse_t resp);
    rt_err_t (*append_tool_message)(struct Context *self, const char *tool_call_id, Messages_t messages);
    rt_err_t (*clear_message)(struct Context *self);
    rt_err_t (*trim_context)(struct Context *self, int keep_rounds);
} Context, *Context_t;

/*
 * @brief 获取角色类型字符串
 * @param type 角色类型枚举
 * @return 角色字符串 ("system"/"user"/"assistant"/"tool")
 */
char *get_role_string(RoleType type);

/*
 * @brief 创建上下文管理器
 * @return Context_t 成功返回句柄，失败返回 NULL
 */
Context_t agent_context_create(void);

/*
 * @brief 销毁上下文管理器
 * @param context 上下文句柄
 */
void agent_context_destroy(Context_t context);

#endif /* __AGENT_CONTEXT_H__ */