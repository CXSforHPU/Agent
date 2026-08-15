#include "tool_compare.h"

/*
 * @brief 比较工具：比较两个数的大小
 * @param args_obj 参数 JSON 对象 {a: number, b: number}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_compare(cJSON *args_obj, AgentToolNode_t node)
{
    /* 1. 参数校验 */
    if (!args_obj || !node)
    {
        if (node)
        {
            if (node->ret.messages)
            {
                messages_destroy(node->ret.messages);
                node->ret.messages = RT_NULL;
            }
            node->ret.ret = RT_ERROR;
        }
        return;
    }

    /* 2. 释放旧消息 */
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    /* 3. 解析参数 */
    cJSON *a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON *b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");

    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    /* 4. 执行比较 */
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    char ret_buffer[64] = {0};

    if (a > b)
    {
        rt_snprintf(ret_buffer, sizeof(ret_buffer), "%f is greater than %f", a, b);
    }
    else if (a < b)
    {
        rt_snprintf(ret_buffer, sizeof(ret_buffer), "%f is greater than %f", b, a);
    }
    else
    {
        rt_snprintf(ret_buffer, sizeof(ret_buffer), "%f is equal to %f", a, b);
    }

    /* 5. 创建消息 */
    node->ret.messages = messages_create(1);
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}