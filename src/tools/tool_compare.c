#include "tool_compare.h"

/* Number comparison tool */
void tool_compare(cJSON* args_obj, AgentToolNode_t node)
{
    memset(node->ret.content, 0, sizeof(node->ret.content));
    cJSON* a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON* b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");
    if (!cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;

    if (a > b)
    {
        rt_snprintf(node->ret.content, sizeof(node->ret.content), "%f is greater than %f", a, b);
    }
    else if (a < b)
    {
        rt_snprintf(node->ret.content, sizeof(node->ret.content), "%f is greater than %f", b, a);
    }
    else
    {
        rt_snprintf(node->ret.content, sizeof(node->ret.content), "%f is equal to %f", a, b);
    }
    node->ret.ret = RT_EOK;
    return;
}