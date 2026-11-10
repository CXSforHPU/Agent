#ifndef __AGENT_FSM_H__
#define __AGENT_FSM_H__

#include <rtthread.h>
#include "cJSON.h"
#include "chat.h"
#include "context.h"
#include "tool_base.h"

/* 一次请求最多跑几轮工具循环，值由 Kconfig 的 PKG_AGENT_TOOL_LOOP_MAX 决定 */
#ifdef PKG_AGENT_TOOL_LOOP_MAX
#define AGENT_LOOP_MAX        PKG_AGENT_TOOL_LOOP_MAX
#else
#define AGENT_LOOP_MAX        6
#endif
/* 一次 LLM 请求最多重试几次 */
#define AGENT_CHAT_RETRY_MAX  3
/* 单次请求的 token 上限 */
#define AGENT_CHAT_MAX_TOKENS (32 * 1024)

/*
 * @brief 状态机的节点
 * @note  每个节点做完本状态的事，返回下一个状态，也就是状态图里的一条边
 */
typedef enum AgentFsmState
{
    AGENT_FSM_INIT = 0,     /* 初始化：清轨迹与护栏计数 */
    AGENT_FSM_THINK,        /* LLM 决策器：请求模型 */
    AGENT_FSM_VALIDATE,     /* 校验动作格式与参数 */
    AGENT_FSM_ACT,          /* 工具路由器：执行工具 */
    AGENT_FSM_OBSERVE,      /* 观察记录器：结果写回轨迹 */
    AGENT_FSM_GUARD,        /* 护栏：轮次上限与重复动作 */
    AGENT_FSM_FINISH,       /* 收尾：输出最终回答 */
    AGENT_FSM_DONE,         /* 结束 */
    AGENT_FSM_STATE_MAX
} AgentFsmState_t;

/* 当前动作的种类 */
typedef enum AgentFsmAction
{
    AGENT_FSM_ACTION_NONE = 0,
    AGENT_FSM_ACTION_ERROR,      /* 参数或工具非法，记一条错误观察 */
    AGENT_FSM_ACTION_DUPLICATE,  /* 重复调用，记一条「结果未变」观察 */
    AGENT_FSM_ACTION_EXECUTE     /* 正常执行 */
} AgentFsmAction_t;

/*
 * @brief 状态机依赖的输出通道
 * @note  状态机不碰 mailbox 和 TTS，这些由上层实现
 */
typedef struct AgentFsmOps
{
    void (*emit_answer)(Context_t context, ChatResponse_t resp, void (*on_context)(const char *text));
    void (*notify)(const char *text, void (*on_context)(const char *text));
} AgentFsmOps, *AgentFsmOps_t;

/*
 * @brief 全局状态：目标、历史轨迹、当前轮次与终止状态
 */
typedef struct AgentFsm
{
    /* 依赖 */
    Context_t context;
    cJSON *tools;
    const AgentFsmOps *ops;
    void (*on_reasoning)(const char *text);
    void (*on_tool_call)(const char *text);
    void (*on_context)(const char *text);

    /* 轮次与终止 */
    int loop_max;
    int chat_retry_max;
    int loop_cnt;         /* 当前轮次 */
    int attempt;          /* 当前请求已重试几次 */
    int dup_rounds;       /* 连续无效轮数 */
    rt_bool_t answered;   /* 是否已给出回答 */
    ChatResponse_t resp;  /* 当前模型回复 */

    /* 工具调用进度 */
    int tool_count;
    int tool_index;
    int dup_hits;         /* 本轮重复调用次数 */
    int over_hits;        /* 本轮超量调用次数 */

    /* 当前动作 */
    int action_kind;
    const char *action_id;
    const char *action_name;
    const char *action_error;
    char action_error_buf[96];
    cJSON *action_args;
    AgentToolNode_t action_tool;
    Messages_t action_messages;
} AgentFsm_t;

/*
 * @brief 跑一轮对话的状态机
 * @param fsm 全局状态，由调用方填好依赖后传入
 */
void agent_fsm_run(AgentFsm_t *fsm);

#endif /* __AGENT_FSM_H__ */
