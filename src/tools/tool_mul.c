#include "tool_mul.h"

/* Multiplication tool */
void tool_mul(cJSON* args_obj, AgentToolNode_t node)
{
    // 1. 参数校验
    if (!args_obj || !node) {
        if (node) {
            if (node->ret.messages) {
                messages_destroy(node->ret.messages);
                node->ret.messages = RT_NULL;
            }
            node->ret.ret = RT_ERROR;
        }
        return;
    }

    // 2. 释放旧消息
    if (node->ret.messages) {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    // 3. 解析并校验参数
    cJSON* a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON* b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");

    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json)) {
        node->ret.ret = RT_ERROR;
        return;
    }

    // 4. 计算乘积
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    char ret_buffer[64] = {0};
    rt_snprintf(ret_buffer, sizeof(ret_buffer), "%f", a * b);

    // 5. 创建返回消息（文本类型）
    node->ret.messages = messages_create(1);
    messages_append(node->ret.messages,TYPE_TEXT,ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}