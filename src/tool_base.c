#include "tool_base.h"

static AgentToolList* head=RT_NULL;
static cJSON* AgentTools = RT_NULL;


rt_err_t AgentToolListCreate(){
    head = (AgentToolNode_t)rt_malloc(sizeof(AgentToolList));
    if (!head)
    {
        /* code */
        LOG_E("head malloc failed.");
        return RT_ERROR;
    }

    head->ret.ret=RT_NULL;
    head->ret.messages = messages_create(0);

    head->tool_obj=RT_NULL;
    head->execute_func=RT_NULL;
    head->next = RT_NULL;

    return RT_EOK;
}

rt_err_t AppendTool(cJSON* tool_obj, void(*execute_func)(cJSON* param,struct AgentToolList* node)){
    if (!head)
    {
        LOG_E("tool list not initialized, call AgentToolListCreate first");
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

/**
 * @brief 从tool对象中获取function.name字符串，安全空保护
 * @param tool_obj 入参cJSON对象
 * @return 成功返回函数字符串；失败返回NULL
 */
static const char* GetFuncName(cJSON* tool_obj)
{
    if (!tool_obj)
    {
        return RT_NULL;
    }
    cJSON* func_obj = cJSON_GetObjectItemCaseSensitive(tool_obj, "function");
    if (!func_obj)
    {
        return RT_NULL;
    }
    cJSON* name_item = cJSON_GetObjectItemCaseSensitive(func_obj, "name");
    if (!name_item || !cJSON_IsString(name_item))
    {
        return RT_NULL;
    }
    return name_item->valuestring;
}


/**
 * @brief 根据tool_obj内function.name查找注册的工具节点
 * @param tool_obj 待匹配工具JSON对象
 * @return 找到返回节点指针，未找到返回RT_NULL
 */
AgentToolNode_t SearchAgentToolNode(cJSON* tool_obj)
{
    if (!head)
    {
        return RT_NULL;
    }

    const char* target_name = GetFuncName(tool_obj);
    if (!target_name)
    {
        return RT_NULL;
    }

    AgentToolNode_t p = head;
    while (p != RT_NULL)
    {
        const char* node_name = GetFuncName(p->tool_obj);
        /* node_name为NULL直接跳过该节点 */
        if (node_name && (rt_strcmp(node_name, target_name) == 0))
        {
            return p;
        }
        p = p->next;
    }
    return RT_NULL;
}


cJSON* BulitToolsJson()
{
    AgentToolNode_t p;

    if (!head)
    {
        LOG_E("tool list not initialized, call AgentToolListCreate first");
        return RT_NULL;
    }

    /* 如果已构建过，直接返回，避免重复添加导致工具列表膨胀 */
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

cJSON* GetAgentTools(){
    return AgentTools;
}

void FreeAgentToolNode(AgentToolNode_t node)
{
    cJSON_Delete(node->tool_obj);
    rt_free(node);
}

