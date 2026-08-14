#include "tool_add.h"

void tool_add(cJSON* args_obj, AgentToolNode_t node)
{
    // 1. 参数校验
    if (!args_obj || !node) {
        if (node) {
            if (node->ret.message) {
                message_destroy(node->ret.message);
                node->ret.message = RT_NULL;
            }
            node->ret.ret = RT_ERROR;
        }
        return;
    }

    // 2. 释放旧消息
    if (node->ret.message) {
        message_destroy(node->ret.message);
        node->ret.message = RT_NULL;
    }

    // 3. 解析并校验参数
    cJSON* a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON* b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");

    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json)) {
        node->ret.ret = RT_ERROR;
        return;   // node->ret.message 已为空
    }

    // 4. 计算
    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    char ret_buffer[64] = {0};
    rt_snprintf(ret_buffer, sizeof(ret_buffer), "计算结果: %f", a + b);

    // 5. 创建返回消息
    node->ret.message = message_create(TYPE_TEXT, ret_buffer, 1);
    node->ret.ret = (node->ret.message) ? RT_EOK : RT_ERROR;

}
