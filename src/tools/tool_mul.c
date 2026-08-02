#include "tool_mul.h"

/* Multiplication tool */
void tool_mul(cJSON* args_obj, AgentToolNode_t node)
{
    rt_memset(node->ret.content, 0, sizeof(node->ret.content));
    cJSON* a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON* b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");
    if (!cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    rt_snprintf(node->ret.content, sizeof(node->ret.content), "%f", a * b);
    node->ret.ret = RT_EOK;
    return;
}