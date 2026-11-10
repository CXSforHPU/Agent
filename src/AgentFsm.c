#include "AgentFsm.h"
#include "MessageHub.h"
#include "utils.h"

#define LOG_TAG "Agent.Fsm"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * 护栏：重复调用检测
 * 模型有时会反复调用同一个工具，工具返回空结果时就会无限重试。这里记录最近
 * 几次调用签名，重复的直接跳过执行并把「结果不会变」告诉模型；连续整轮重复
 * 则强制生成文本回答。
 */
#define AGENT_CALL_SIG_LEN   192
#define AGENT_CALL_SIG_KEEP  6
#define AGENT_TOOL_NAME_LEN  32
#define AGENT_TOOL_HITS_MAX  3

static char g_call_sig[AGENT_CALL_SIG_KEEP][AGENT_CALL_SIG_LEN];
static int g_call_sig_count = 0;

/*
 * @brief  清空工具调用签名记录
 */
static void agent_fsm_call_sig_reset(void)
{
    g_call_sig_count = 0;
}

/*
 * @brief  查一个调用签名是否已经出现过
 * @param  sig 调用签名，工具名与参数拼成的字符串
 * @return RT_TRUE 出现过
 */
static rt_bool_t agent_fsm_call_sig_seen(const char *sig)
{
    int i;

    for (i = 0; i < g_call_sig_count; i++)
    {
        if (rt_strcmp(g_call_sig[i], sig) == 0)
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

/*
 * @brief  记下一个调用签名，满了就把最旧的一条挤掉
 * @param  sig 调用签名
 */
static void agent_fsm_call_sig_add(const char *sig)
{
    if (g_call_sig_count < AGENT_CALL_SIG_KEEP)
    {
        rt_strncpy(g_call_sig[g_call_sig_count], sig, AGENT_CALL_SIG_LEN - 1);
        g_call_sig[g_call_sig_count][AGENT_CALL_SIG_LEN - 1] = '\0';
        g_call_sig_count++;
        return;
    }

    rt_memmove(g_call_sig[0], g_call_sig[1], (AGENT_CALL_SIG_KEEP - 1) * AGENT_CALL_SIG_LEN);
    rt_strncpy(g_call_sig[AGENT_CALL_SIG_KEEP - 1], sig, AGENT_CALL_SIG_LEN - 1);
    g_call_sig[AGENT_CALL_SIG_KEEP - 1][AGENT_CALL_SIG_LEN - 1] = '\0';
}

static char g_tool_hits_name[AGENT_CALL_SIG_KEEP][AGENT_TOOL_NAME_LEN];
static int g_tool_hits_count[AGENT_CALL_SIG_KEEP];
static int g_tool_hits_used = 0;

/*
 * @brief  清空工具调用计数
 */
static void agent_fsm_tool_hits_reset(void)
{
    g_tool_hits_used = 0;
    rt_memset(g_tool_hits_count, 0, sizeof(g_tool_hits_count));
}

/*
 * @brief  记录一次工具调用
 * @param  name 工具名
 * @return 该工具在本次对话中已被调用的次数，含本次
 */
static int agent_fsm_tool_hit(const char *name)
{
    int i;

    for (i = 0; i < g_tool_hits_used; i++)
    {
        if (rt_strcmp(g_tool_hits_name[i], name) == 0)
        {
            return ++g_tool_hits_count[i];
        }
    }

    if (g_tool_hits_used < AGENT_CALL_SIG_KEEP)
    {
        rt_strncpy(g_tool_hits_name[g_tool_hits_used], name, AGENT_TOOL_NAME_LEN - 1);
        g_tool_hits_name[g_tool_hits_used][AGENT_TOOL_NAME_LEN - 1] = '\0';
        g_tool_hits_count[g_tool_hits_used] = 1;
        g_tool_hits_used++;
    }
    return 1;
}

/* 重复调用时喂回模型的说明 */
#define AGENT_FSM_DUP_TEXT \
    "This exact tool call was already executed in this conversation and its result is " \
    "unchanged (see the previous tool result). Do not call it again - answer the user " \
    "with the information you already have."

/*
 * @brief  给一个工具调用补一条结果消息
 * @param  context      上下文
 * @param  tool_call_id 工具调用 id
 * @param  text         结果文本
 * @note  assistant.tool_calls 中的每个 id 都必须有对应的 tool 消息，
 *        未知工具、参数非法等情况也要回一条，否则下一轮请求会被 API 以 400 拒绝
 */
static void agent_fsm_append_tool_text(Context_t context, const char *tool_call_id, const char *text)
{
    Messages_t messages;

    if (context == RT_NULL || tool_call_id == RT_NULL || text == RT_NULL)
    {
        return;
    }

    messages = messages_create(1);
    if (messages == RT_NULL)
    {
        return;
    }
    if (messages_append(messages, TYPE_TEXT, text) == RT_EOK)
    {
        context->append_tool_message(context, tool_call_id, messages);
    }
    messages_destroy(messages);
}

/*
 * @brief  释放本轮响应
 * @param  fsm 状态机
 */
static void agent_fsm_free_response(AgentFsm_t *fsm)
{
    if (fsm->resp != RT_NULL)
    {
        chat_response_free(fsm->resp);
        fsm->resp = RT_NULL;
    }
}

/*
 * @brief  取状态名
 * @param  state 状态
 * @return 状态名字符串
 */
static const char *agent_fsm_state_name(AgentFsmState_t state)
{
    switch (state)
    {
    case AGENT_FSM_INIT:     return "INIT";
    case AGENT_FSM_THINK:    return "THINK";
    case AGENT_FSM_VALIDATE: return "VALIDATE";
    case AGENT_FSM_ACT:      return "ACT";
    case AGENT_FSM_OBSERVE:  return "OBSERVE";
    case AGENT_FSM_GUARD:    return "GUARD";
    case AGENT_FSM_FINISH:   return "FINISH";
    default:                 return "DONE";
    }
}

/* 节点：每个节点返回下一个状态 */

/*
 * @brief  初始化：清空轨迹与护栏计数
 * @param  fsm 状态机
 * @return 下一个状态 THINK
 */
static AgentFsmState_t agent_fsm_state_init(AgentFsm_t *fsm)
{
    fsm->loop_cnt = 0;
    fsm->attempt = 0;
    fsm->dup_rounds = 0;
    fsm->answered = RT_FALSE;
    fsm->resp = RT_NULL;
    fsm->tool_count = 0;
    fsm->tool_index = 0;
    fsm->dup_hits = 0;
    fsm->over_hits = 0;
    fsm->action_kind = AGENT_FSM_ACTION_NONE;
    fsm->action_id = RT_NULL;
    fsm->action_name = RT_NULL;
    fsm->action_error = RT_NULL;
    fsm->action_args = RT_NULL;
    fsm->action_tool = RT_NULL;
    fsm->action_messages = RT_NULL;

    agent_fsm_call_sig_reset();
    agent_fsm_tool_hits_reset();
    return AGENT_FSM_THINK;
}

/*
 * @brief  问模型要下一步动作
 * @param  fsm 状态机
 * @return 下一个状态，模型给出回答时返回 FINISH
 */
static AgentFsmState_t agent_fsm_state_think(AgentFsm_t *fsm)
{
    fsm->loop_cnt++;

    /* 请求失败重试：TLS/网络抖动（如 mbedtls NET_RECV_FAILED、对端重置）不应直接
       丢弃整轮对话；已流式输出的部分内容在重试时可能重复打印，属可接受的代价 */
    for (fsm->attempt = 1; fsm->attempt <= fsm->chat_retry_max; fsm->attempt++)
    {
        fsm->resp = chat(fsm->context->message, fsm->tools, AGENT_CHAT_MAX_TOKENS,
                         fsm->on_reasoning, fsm->on_tool_call, fsm->on_context);
        if (fsm->resp != RT_NULL)
        {
            break;
        }
        if (fsm->attempt < fsm->chat_retry_max)
        {
            LOG_W("Chat request failed (attempt %d/%d), retry in %d ms",
                  fsm->attempt, fsm->chat_retry_max, 1000 * fsm->attempt);
            rt_thread_mdelay(1000 * fsm->attempt);
        }
    }

    if (fsm->resp == RT_NULL)
    {
        LOG_E("Chat request failed after %d attempts, terminating this case", fsm->chat_retry_max);
        fsm->ops->notify("(LLM request failed after retries, please try again)", fsm->on_context);
        fsm->answered = RT_TRUE;
        return AGENT_FSM_FINISH;
    }

    /* 没有 tool_calls，这一轮给出的就是最终回答 */
    if (cJSON_GetArraySize(fsm->resp->tool_call) == 0)
    {
        fsm->answered = RT_TRUE;
        return AGENT_FSM_FINISH;
    }

    /* 先存带 tool_calls 的 assistant 消息再执行工具：tool 消息必须能对上
       前一条 assistant 消息里的 tool_calls，顺序反了会被 API 拒 */
    fsm->context->append_assistant_message(fsm->context, fsm->resp);

    fsm->tool_count = cJSON_GetArraySize(fsm->resp->tool_call);
    fsm->tool_index = 0;
    fsm->dup_hits = 0;
    fsm->over_hits = 0;
    LOG_I("\nDetected %d tool invocation(s)\n", fsm->tool_count);
    return AGENT_FSM_VALIDATE;
}

/*
 * @brief  校验动作格式与参数，决定这个调用是执行、跳过还是记错误
 * @param  fsm 状态机
 * @return 下一个状态
 */
static AgentFsmState_t agent_fsm_state_validate(AgentFsm_t *fsm)
{
    cJSON *tc_item;
    cJSON *id_node;
    cJSON *tc_func;
    cJSON *name_node;
    cJSON *args_node;
    const char *func_name;
    const char *args_str;
    AgentToolNode_t tool_node;
    char sig[AGENT_CALL_SIG_LEN];

    if (fsm->tool_index >= fsm->tool_count)
    {
        return AGENT_FSM_GUARD;
    }

    tc_item = cJSON_GetArrayItem(fsm->resp->tool_call, fsm->tool_index);
    if (!tc_item)
    {
        fsm->tool_index++;
        return AGENT_FSM_VALIDATE;
    }

    id_node = cJSON_GetObjectItem(tc_item, "id");
    /* id 缺失/非字符串时兜底，保证 append_tool_message 的 tool_call_id 有效 */
    fsm->action_id = (id_node && cJSON_IsString(id_node)) ? id_node->valuestring : "call_0";

    tc_func = cJSON_GetObjectItemCaseSensitive(tc_item, "function");
    if (!tc_func || !cJSON_IsObject(tc_func))
    {
        LOG_I("Tool call %d missing function field, skip\n", fsm->tool_index);
        fsm->action_kind = AGENT_FSM_ACTION_ERROR;
        fsm->action_error = "Tool call is missing the function field.";
        return AGENT_FSM_OBSERVE;
    }

    name_node = cJSON_GetObjectItemCaseSensitive(tc_func, "name");
    args_node = cJSON_GetObjectItemCaseSensitive(tc_func, "arguments");
    if (!name_node || !cJSON_IsString(name_node) || !args_node || !cJSON_IsString(args_node))
    {
        LOG_I("Tool call %d invalid name/arguments, skip\n", fsm->tool_index);
        fsm->action_kind = AGENT_FSM_ACTION_ERROR;
        fsm->action_error = "Tool call has an invalid function name or arguments.";
        return AGENT_FSM_OBSERVE;
    }

    func_name = name_node->valuestring;
    args_str = args_node->valuestring;
    fsm->action_name = func_name;

    tool_node = search_agent_tool_node(tc_item);
    if (!tool_node)
    {
        rt_snprintf(fsm->action_error_buf, sizeof(fsm->action_error_buf),
                    "Unknown tool: %s", func_name);
        LOG_W("%s\n", fsm->action_error_buf);
        fsm->action_kind = AGENT_FSM_ACTION_ERROR;
        fsm->action_error = fsm->action_error_buf;
        return AGENT_FSM_OBSERVE;
    }

    /* 重复调用检测：同参数调过同一个工具，结果不会变。直接跳过执行并把
       「结果不变、请直接回答」喂回模型，否则工具返回空内容时模型会无限重试 */
    rt_snprintf(sig, sizeof(sig), "%s|%s", func_name, args_str);
    if (agent_fsm_call_sig_seen(sig))
    {
        LOG_W("duplicated tool call skipped: %s(%s)", func_name, args_str);
        fsm->dup_hits++;
        fsm->action_kind = AGENT_FSM_ACTION_DUPLICATE;
        return AGENT_FSM_OBSERVE;
    }
    agent_fsm_call_sig_add(sig);

    /* 同一工具被反复调用（哪怕参数在变）：超过上限就计入无效轮次 */
    if (agent_fsm_tool_hit(func_name) > AGENT_TOOL_HITS_MAX)
    {
        LOG_W("tool %s called too many times in this case, treating as unproductive",
              func_name);
        fsm->over_hits++;
    }

    fsm->action_args = cJSON_Parse(args_str);
    if (!fsm->action_args)
    {
        LOG_I("Tool %s parse arguments failed, skip execution\n", func_name);
        fsm->action_kind = AGENT_FSM_ACTION_ERROR;
        fsm->action_error = "Tool arguments are not valid JSON.";
        return AGENT_FSM_OBSERVE;
    }

    fsm->action_tool = tool_node;
    fsm->action_kind = AGENT_FSM_ACTION_EXECUTE;
    return AGENT_FSM_ACT;
}

/*
 * @brief  执行工具
 * @param  fsm 状态机
 * @return 下一个状态 OBSERVE
 */
static AgentFsmState_t agent_fsm_state_act(AgentFsm_t *fsm)
{
    fsm->action_tool->execute_func(fsm->action_args, fsm->action_tool);

    fsm->action_messages = fsm->action_tool->ret.messages;
    /* 工具参数校验失败时 ret.messages 可能为 NULL */
    if (fsm->action_messages == RT_NULL)
    {
        LOG_I("Tool %s returned no result\n", fsm->action_name);
        fsm->action_kind = AGENT_FSM_ACTION_ERROR;
        fsm->action_error = "Tool execution failed (invalid or missing arguments).";
    }
    else
    {
        int i;

        for (i = 0; i < fsm->action_messages->current_size; i++)
        {
            LOG_I("[Local Tool %s Execution Result] type %s, result %s\n", fsm->action_name,
                  get_agent_content_type(messages_get_type_idx(fsm->action_messages, i)),
                  messages_get_content_idx(fsm->action_messages, i));
        }
    }
    return AGENT_FSM_OBSERVE;
}

/*
 * @brief  把工具返回或错误写回轨迹
 * @param  fsm 状态机
 * @return 下一个状态 VALIDATE
 */
static AgentFsmState_t agent_fsm_state_observe(AgentFsm_t *fsm)
{
    switch (fsm->action_kind)
    {
    case AGENT_FSM_ACTION_ERROR:
        agent_fsm_append_tool_text(fsm->context, fsm->action_id, fsm->action_error);
        break;
    case AGENT_FSM_ACTION_DUPLICATE:
        agent_fsm_append_tool_text(fsm->context, fsm->action_id, AGENT_FSM_DUP_TEXT);
        break;
    case AGENT_FSM_ACTION_EXECUTE:
        fsm->context->append_tool_message(fsm->context, fsm->action_id, fsm->action_messages);
        break;
    default:
        break;
    }

    if (fsm->action_args != RT_NULL)
    {
        cJSON_Delete(fsm->action_args);
        fsm->action_args = RT_NULL;
    }

    fsm->tool_index++;
    return AGENT_FSM_VALIDATE;
}

/*
 * @brief  护栏：看轮次上限与重复动作，决定是否进下一轮
 * @param  fsm 状态机
 * @return 下一个状态
 */
static AgentFsmState_t agent_fsm_state_guard(AgentFsm_t *fsm)
{
    /* 整轮都是重复调用（或同一工具反复调用到了上限），说明模型不认可已有结果。
       连续两轮如此就强制它用文本回答，避免一直卡在工具调用上不回复用户 */
    if (fsm->tool_count > 0 && (fsm->dup_hits + fsm->over_hits) >= fsm->tool_count)
    {
        fsm->dup_rounds++;
        if (fsm->dup_rounds >= 2)
        {
            LOG_W("repeated tool calls detected, forcing a final answer");
            agent_fsm_free_response(fsm);
            return AGENT_FSM_FINISH;
        }
    }
    else
    {
        fsm->dup_rounds = 0;
    }

    agent_fsm_free_response(fsm);

    if (fsm->loop_cnt >= fsm->loop_max)
    {
        return AGENT_FSM_FINISH;
    }
    return AGENT_FSM_THINK;
}

/*
 * @brief  收尾：输出最终回答，或者再问一次模型逼出纯文本
 * @param  fsm 状态机
 * @return 下一个状态 DONE
 */
static AgentFsmState_t agent_fsm_state_finish(AgentFsm_t *fsm)
{
    if (fsm->resp != RT_NULL && fsm->answered)
    {
        fsm->ops->emit_answer(fsm->context, fsm->resp, fsm->on_context);
        agent_fsm_free_response(fsm);
    }

    if (fsm->loop_cnt >= fsm->loop_max)
    {
        LOG_E("Maximum tool loop count %d reached, forcing a final answer\n", fsm->loop_max);
        agent_fsm_free_response(fsm);
    }

    /* 仍没给出回答时（到了轮次上限，或因重复调用跳出），最后不带工具再问一次
       模型，逼出一段纯文本；再失败就给一句明确提示，保证这一轮有交代 */
    if (!fsm->answered)
    {
        fsm->resp = chat(fsm->context->message, RT_NULL, AGENT_CHAT_MAX_TOKENS,
                         fsm->on_reasoning, RT_NULL, fsm->on_context);
        if (fsm->resp != RT_NULL)
        {
            fsm->ops->emit_answer(fsm->context, fsm->resp, fsm->on_context);
            agent_fsm_free_response(fsm);
        }
        else
        {
            LOG_E("forced final answer failed");
            fsm->ops->notify("(stopped after too many tool calls without a usable result - "
                             "please restate your question)", fsm->on_context);
        }
    }
    return AGENT_FSM_DONE;
}

/*
 * @brief  终止节点
 * @param  fsm 状态机
 * @return 下一个状态 DONE
 */
static AgentFsmState_t agent_fsm_state_done(AgentFsm_t *fsm)
{
    return AGENT_FSM_DONE;
}

typedef AgentFsmState_t (*agent_fsm_node_t)(AgentFsm_t *fsm);

/* 状态图：节点表，每个节点返回下一条边 */
static const agent_fsm_node_t g_agent_fsm_nodes[AGENT_FSM_STATE_MAX] = {
    [AGENT_FSM_INIT]     = agent_fsm_state_init,
    [AGENT_FSM_THINK]    = agent_fsm_state_think,
    [AGENT_FSM_VALIDATE] = agent_fsm_state_validate,
    [AGENT_FSM_ACT]      = agent_fsm_state_act,
    [AGENT_FSM_OBSERVE]  = agent_fsm_state_observe,
    [AGENT_FSM_GUARD]    = agent_fsm_state_guard,
    [AGENT_FSM_FINISH]   = agent_fsm_state_finish,
    [AGENT_FSM_DONE]     = agent_fsm_state_done,
};

/*
 * @brief  跑完整个状态机
 * @param  fsm 状态机
 */
void agent_fsm_run(AgentFsm_t *fsm)
{
    AgentFsmState_t state = AGENT_FSM_INIT;

    while (state != AGENT_FSM_DONE)
    {
        AgentFsmState_t next = g_agent_fsm_nodes[state](fsm);

        LOG_I("[fsm] %s -> %s\n", agent_fsm_state_name(state), agent_fsm_state_name(next));
        state = next;
    }
}
