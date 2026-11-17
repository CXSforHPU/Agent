#ifndef __AGENT_CONTEXT_H__
#define __AGENT_CONTEXT_H__
#include "rtconfig.h"
#include <rtthread.h>
#include "prompt.h"
#include "MessageHub.h"
#include "chat.h"
#include "utils.h"

#include "core/context_msg.h"

/* 摘要正文上限：压缩指令要求模型写 1200 字节以内，这里留出余量 */
#define AGENT_CONTEXT_SUMMARY_MAX 1536

/* 上下文管理器结构 */
typedef struct Context
{
    rt_uint8_t *arena;   /* 历史记录连续存储区 */
    rt_size_t capacity;  /* arena 容量，字节 */
    rt_size_t base;      /* 最旧记录的起点，前缀丢弃只改这个值 */
    rt_size_t used;      /* 已用区末端 */
    int count;           /* 历史记录条数，不含系统提示词与摘要 */

    char *system_prompt; /* 系统提示词，单独分配 */
    rt_bool_t has_summary;
    char summary[AGENT_CONTEXT_SUMMARY_MAX];

    rt_err_t (*build_system_prompt)(struct Context *self);
    rt_err_t (*append_user_message)(struct Context *self, Messages_t messages);
    rt_err_t (*append_assistant_message)(struct Context *self, ChatResponse_t resp);
    rt_err_t (*append_tool_message)(struct Context *self, const char *tool_call_id, Messages_t messages);
    rt_err_t (*clear_message)(struct Context *self);
    rt_err_t (*compress_context)(struct Context *self, int threshold);
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

/*
 * @brief 丢掉最旧的若干条历史记录（摘要与会话状态保留）
 * @param self  上下文
 * @param count 丢弃条数
 * @return RT_EOK 成功
 */
rt_err_t agent_context_drop_front(Context_t self, int count);

#endif /* __AGENT_CONTEXT_H__ */
