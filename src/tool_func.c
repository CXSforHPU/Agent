#include "tool_func.h"

static rt_err_t init_flag = RT_FALSE;

/*
 * @brief 初始化所有工具（注册到工具链表）
 */
void init_tools(void)
{
    if (init_flag)
    {
        return;
    }

    agent_tool_list_create();

    /* ========== add ========== */
    cJSON *props_add = cJSON_CreateObject();
    create_property(props_add, "a", "double", "First number");
    create_property(props_add, "b", "double", "Second number");

    cJSON *required_add = cJSON_CreateArray();
    cJSON_AddItemToArray(required_add, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_add, cJSON_CreateString("b"));

    cJSON *params_add = create_param_obj(props_add, required_add);
    cJSON *add_func = create_tool_item("add", "Calculate the sum of two numbers", params_add);
    append_tool(add_func, tool_add);

    /* ========== mul ========== */
    cJSON *props_mul = cJSON_CreateObject();
    create_property(props_mul, "a", "double", "First multiplier");
    create_property(props_mul, "b", "double", "Second multiplier");

    cJSON *required_mul = cJSON_CreateArray();
    cJSON_AddItemToArray(required_mul, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_mul, cJSON_CreateString("b"));

    cJSON *params_mul = create_param_obj(props_mul, required_mul);
    cJSON *mul_func = create_tool_item("mul", "Calculate the product of two numbers", params_mul);
    append_tool(mul_func, tool_mul);

    /* ========== compare ========== */
    cJSON *props_cmp = cJSON_CreateObject();
    create_property(props_cmp, "a", "double", "Number A to compare");
    create_property(props_cmp, "b", "double", "Number B to compare");

    cJSON *required_cmp = cJSON_CreateArray();
    cJSON_AddItemToArray(required_cmp, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_cmp, cJSON_CreateString("b"));

    cJSON *params_cmp = create_param_obj(props_cmp, required_cmp);
    cJSON *cmp_func = create_tool_item("compare", "Compare the size of two numbers", params_cmp);
    append_tool(cmp_func, tool_compare);

    /* Build tool list json */
    build_tools_json();
    init_flag = RT_TRUE;
}

MSH_CMD_EXPORT(init_tools, init_tools);