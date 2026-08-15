#include "tool_base.h"

#define LOG_TAG "Agent.tool_base"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static AgentToolList *head = RT_NULL;
static cJSON *AgentTools = RT_NULL;

/*
 * @brief 创建工具链表头节点
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t agent_tool_list_create(void)
{
    head = (AgentToolNode_t)rt_malloc(sizeof(AgentToolList));
    if (!head)
    {
        LOG_E("head malloc failed.");
        return RT_ERROR;
    }

    head->ret.ret = RT_EOK;
    head->ret.messages = messages_create(0);

    head->tool_obj = RT_NULL;
    head->execute_func = RT_NULL;
    head->next = RT_NULL;

    return RT_EOK;
}

/*
 * @brief 追加工具到链表尾部
 * @param tool_obj      工具 JSON 对象
 * @param execute_func  工具执行函数
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t append_tool(cJSON *tool_obj, void (*execute_func)(cJSON *param, struct AgentToolList *node))
{
    if (!head)
    {
        LOG_E("tool list not initialized, call agent_tool_list_create first");
        return RT_ERROR;
    }

    AgentToolNode_t node = (AgentToolNode_t)rt_malloc(sizeof(AgentToolList));
    if (!node)
    {
        LOG_E("node malloc failed.");
        return RT_ERROR;
    }

    AgentToolNode_t p = head;
    while (p->next != RT_NULL)
    {
        p = p->next;
    }

    node->tool_obj = tool_obj;
    node->ret.messages = messages_create(0);
    node->ret.ret = RT_ERROR;
    node->execute_func = execute_func;
    node->next = RT_NULL;

    p->next = node;
    return RT_EOK;
}

/*
 * @brief 从 tool_obj 中获取 function.name 字符串
 * @param tool_obj 工具 JSON 对象
 * @return 函数名字符串，失败返回 NULL
 */
static const char *get_func_name(cJSON *tool_obj)
{
    if (!tool_obj)
    {
        return RT_NULL;
    }
    cJSON *func_obj = cJSON_GetObjectItemCaseSensitive(tool_obj, "function");
    if (!func_obj)
    {
        return RT_NULL;
    }
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(func_obj, "name");
    if (!name_item || !cJSON_IsString(name_item))
    {
        return RT_NULL;
    }
    return name_item->valuestring;
}

/*
 * @brief 根据 tool_obj 内 function.name 查找注册的工具节点
 * @param tool_obj 待匹配工具 JSON 对象
 * @return 找到返回节点指针，未找到返回 NULL
 */
AgentToolNode_t search_agent_tool_node(cJSON *tool_obj)
{
    if (!head)
    {
        return RT_NULL;
    }

    const char *target_name = get_func_name(tool_obj);
    if (!target_name)
    {
        return RT_NULL;
    }

    AgentToolNode_t p = head;
    while (p != RT_NULL)
    {
        const char *node_name = get_func_name(p->tool_obj);
        if (node_name && (rt_strcmp(node_name, target_name) == 0))
        {
            return p;
        }
        p = p->next;
    }
    return RT_NULL;
}

/*
 * @brief 构建所有工具的 JSON 数组
 * @return 工具 JSON 数组指针，失败返回 NULL
 */
cJSON *build_tools_json(void)
{
    AgentToolNode_t p;

    if (!head)
    {
        LOG_E("tool list not initialized, call agent_tool_list_create first");
        return RT_NULL;
    }

    /* 如果已构建过，直接返回 */
    if (cJSON_IsArray(AgentTools) && cJSON_GetArraySize(AgentTools) > 0)
    {
        return AgentTools;
    }

    /* 初次初始化 */
    if (!cJSON_IsArray(AgentTools))
    {
        AgentTools = cJSON_CreateArray();
    }

    p = head->next;
    while (p != RT_NULL)
    {
        cJSON_AddItemToArray(AgentTools, p->tool_obj);
        p = p->next;
    }

    return AgentTools;
}

/*
 * @brief 获取已构建的工具 JSON 数组
 * @return 工具 JSON 数组指针
 */
cJSON *get_agent_tools(void)
{
    return AgentTools;
}

/*
 * @brief 释放工具节点（仅释放节点自身与 ret.messages）
 * @note tool_obj 的所有权在 build_tools_json 时已移交给 AgentTools 数组，
 *       由 agent_tool_list_destroy 统一释放，此处不得再 cJSON_Delete(tool_obj)
 * @param node 要释放的节点
 */
void free_agent_tool_node(AgentToolNode_t node)
{
    if (!node)
    {
        return;
    }
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }
    rt_free(node);
}

/*
 * @brief 销毁整个工具链表（含 AgentTools 数组及各节点残留的 ret.messages）
 * @note 工具链为全局单例，仅在 agent 清理（cleanup_agent）时调用；
 *       调用后需复位 tool_func 的注册标志，再次进入时重新注册
 */
void agent_tool_list_destroy(void)
{
    AgentToolNode_t p = head;
    AgentToolNode_t next = RT_NULL;

    while (p != RT_NULL)
    {
        next = p->next;
        free_agent_tool_node(p);
        p = next;
    }
    head = RT_NULL;

    if (AgentTools)
    {
        cJSON_Delete(AgentTools);
        AgentTools = RT_NULL;
    }
}