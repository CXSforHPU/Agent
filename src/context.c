#include "context.h"

#define LOG_TAG "Agent.context"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static char AgentRole[4][16] = {"system", "user", "assistant", "tool"};

/*
 * @brief 获取角色类型对应的字符串
 * @param type 角色类型枚举
 * @return 角色字符串
 */
char *get_role_string(RoleType type)
{
    return AgentRole[type];
}

/*
 * @brief 构建系统 prompt，添加到消息列表头部
 * @param self 上下文管理器
 * @return RT_EOK 成功
 */
static rt_err_t __build_system_prompt(Context_t self)
{
    cJSON *system_prompt = cJSON_CreateObject();
    cJSON_AddStringToObject(system_prompt, "role", get_role_string(ROLE_SYSTEM));
    cJSON_AddStringToObject(system_prompt, "content", get_system_prompt());

    cJSON_AddItemToArray(self->message, system_prompt);

    return RT_EOK;
}

/*
 * @brief 追加用户消息到上下文
 * @param self     上下文管理器
 * @param messages 用户消息数组
 * @return RT_EOK 成功，RT_ERROR 失败
 */
static rt_err_t __append_user_message(Context_t self, Messages_t messages)
{
    cJSON *user_message = RT_NULL;
    cJSON *content_obj = RT_NULL;

    if (!self || !messages || !self->message)
    {
        return -RT_EINVAL;
    }

    user_message = cJSON_CreateObject();
    if (!user_message)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(user_message, "role", get_role_string(ROLE_USER));

    content_obj = to_content(messages);

    cJSON_AddItemToObject(user_message, "content", content_obj);
    cJSON_AddItemToArray(self->message, user_message);

    user_message = RT_NULL;
    content_obj = RT_NULL;
    return RT_EOK;
}

/*
 * @brief 追加助手消息到上下文
 * @param self 上下文管理器
 * @param resp 聊天响应（含 tool_calls 或 content）
 * @return RT_EOK 成功
 */
static rt_err_t __append_assistant_message(Context_t self, ChatResponse_t resp)
{
    /* 无 tool_calls：最终回答 */
    if (cJSON_GetArraySize(resp->tool_call) == 0)
    {
        const char *ans = resp->context ? resp->context : "";
        cJSON *assist_message = cJSON_CreateObject();
        cJSON_AddStringToObject(assist_message, "role", get_role_string(ROLE_ASSISTANT));
        cJSON_AddStringToObject(assist_message, "content", ans);
        cJSON_AddItemToArray(self->message, assist_message);

        return RT_EOK;
    }

    /* 有 tool_calls：保存含 tool_calls 的助手消息 */
    cJSON *assist_tc = cJSON_CreateObject();
    cJSON_AddStringToObject(assist_tc, "role", get_role_string(ROLE_ASSISTANT));
    cJSON_AddItemToObject(assist_tc, "tool_calls", cJSON_Duplicate(resp->tool_call, cJSON_True));
    cJSON_AddItemToArray(self->message, assist_tc);

    return RT_EOK;
}

/*
 * @brief 追加工具返回消息到上下文
 * @param self         上下文管理器
 * @param tool_call_id 工具调用 ID
 * @param messages     工具返回的消息数组
 * @return RT_EOK 成功
 */
static rt_err_t __append_tool_message(Context_t self, const char *tool_call_id, Messages_t messages)
{
    cJSON *tool_msg = cJSON_CreateObject();
    cJSON *content_obj = RT_NULL;
    cJSON_AddStringToObject(tool_msg, "role", get_role_string(ROLE_TOOL));
    cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id);

    content_obj = to_content(messages);

    cJSON_AddItemToObject(tool_msg, "content", content_obj);
    cJSON_AddItemToArray(self->message, tool_msg);

    tool_msg = RT_NULL;
    content_obj = RT_NULL;
    return RT_EOK;
}

/*
 * @brief 清空上下文消息列表
 * @param self 上下文管理器
 * @return RT_EOK 成功
 */
static rt_err_t __clear_message(Context_t self)
{
    if (self->message)
    {
        cJSON_Delete(self->message);
        self->message = cJSON_CreateArray();
    }
    return RT_EOK;
}

/*
 * @brief 裁剪上下文：保留 system prompt + 最近 keep_items 条消息
 * @param self        上下文管理器
 * @param keep_items  保留的消息条数（不含 system prompt）
 * @return RT_EOK 成功
 */
static rt_err_t __trim_context(Context_t self, int keep_items)
{
    int total = cJSON_GetArraySize(self->message);
    if (total <= 1)
        return RT_EOK;

    int history = total - 1;
    if (history <= keep_items)
        return RT_EOK;

    int remove_count = history - keep_items;
    for (int i = 0; i < remove_count; i++)
    {
        cJSON *item = cJSON_DetachItemFromArray(self->message, 1);
        cJSON_Delete(item);
    }
    LOG_D("Trimmed %d old messages, keeping last %d items", remove_count, keep_items);
    return RT_EOK;
}

/*
 * @brief 创建上下文管理器
 * @return Context_t 成功返回句柄，失败返回 NULL
 */
Context_t agent_context_create(void)
{
    Context_t context = (Context_t)rt_malloc(sizeof(Context));
    if (!context)
    {
        LOG_E("context malloc failed.");
        return RT_NULL;
    }
    context->message = cJSON_CreateArray();
    if (context->message == RT_NULL)
    {
        LOG_E("context message create failed.");
        rt_free(context);
        return RT_NULL;
    }

    context->build_system_prompt = __build_system_prompt;
    context->append_user_message = __append_user_message;
    context->append_tool_message = __append_tool_message;
    context->append_assistant_message = __append_assistant_message;
    context->clear_message = __clear_message;
    context->trim_context = __trim_context;

    context->build_system_prompt(context);
    return context;
}

/*
 * @brief 销毁上下文管理器
 * @param context 上下文句柄
 */
void agent_context_destroy(Context_t context)
{
    if (context == RT_NULL)
    {
        return;
    }
    if (context->message)
    {
        cJSON_Delete(context->message);
        context->message = RT_NULL;
    }
    rt_free(context);
}