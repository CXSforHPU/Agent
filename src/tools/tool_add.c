/*
 * 加法工具：计算两个数的和
 * 只做数值运算，不访问任何外设。
 */
#include "tool_add.h"

/*
 * @brief 加法工具：计算两个数的和
 * @param args_obj 参数 JSON 对象 {a: number, b: number}
 * @param node     工具链表节点（用于返回结果）
 */
void tool_add(cJSON *args_obj, AgentToolNode_t node)
{
    if (node == RT_NULL)
    {
        return;
    }

    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    if (args_obj == RT_NULL)
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    cJSON *a_json = cJSON_GetObjectItemCaseSensitive(args_obj, "a");
    cJSON *b_json = cJSON_GetObjectItemCaseSensitive(args_obj, "b");

    if (!a_json || !b_json || !cJSON_IsNumber(a_json) || !cJSON_IsNumber(b_json))
    {
        node->ret.ret = RT_ERROR;
        return;
    }

    double a = a_json->valuedouble;
    double b = b_json->valuedouble;
    char ret_buffer[64] = {0};
    rt_snprintf(ret_buffer, sizeof(ret_buffer), "计算结果: %f", a + b);

    node->ret.messages = messages_create(1);
    messages_append(node->ret.messages, TYPE_TEXT, ret_buffer);
    node->ret.ret = (node->ret.messages) ? RT_EOK : RT_ERROR;
}

/*
 * @brief 填 add 工具的描述
 * @param func 描述 JSON 对象
 */
static void add_desc(cJSON *func)
{
    tool_desc(func, "Calculate the sum of two numbers");
}

/*
 * @brief 填 add 工具的参数表
 * @param props   属性 JSON 对象
 * @param required 必填项数组
 */
static void add_params(cJSON *props, cJSON *required)
{
    create_property(props, "a", "number", "First number");
    create_property(props, "b", "number", "Second number");
    cJSON_AddItemToArray(required, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required, cJSON_CreateString("b"));
}

static const struct agent_tool_def add_tools[] =
{
    {
        "add",
        add_desc,
        add_params,
        tool_add
    }
};

AGENT_TOOL_MODULE_EXPORT(add, add_tools)
