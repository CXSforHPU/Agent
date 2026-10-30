#include "utils/utils_internal.h"

/*
 * @brief 创建工具项 JSON 对象
 * @param func_name  函数名
 * @param desc       函数描述
 * @param params_obj 参数对象
 * @return 工具项 cJSON 对象
 */
cJSON *create_tool_item(
    const char *func_name,
    const char *desc,
    cJSON *params_obj)
{
    cJSON *tool_root = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_root, "type", "function");

    cJSON *func_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(func_obj, "name", func_name);
    cJSON_AddStringToObject(func_obj, "description", desc);
    cJSON_AddItemToObject(func_obj, "parameters", params_obj);

    cJSON_AddItemToObject(tool_root, "function", func_obj);
    return tool_root;
}

/*
 * @brief 创建参数对象 { type: "object", properties: {...}, required: [...] }
 * @param props_obj    属性对象
 * @param required_arr 必填数组
 * @return 参数 cJSON 对象
 */
cJSON *create_param_obj(
    cJSON *props_obj,
    cJSON *required_arr)
{
    cJSON *param_root = cJSON_CreateObject();
    cJSON_AddStringToObject(param_root, "type", "object");
    cJSON_AddItemToObject(param_root, "properties", props_obj);
    cJSON_AddItemToObject(param_root, "required", required_arr);
    return param_root;
}

/*
 * @brief 创建单个属性并添加到属性对象
 * @param props_obj 属性对象
 * @param name      属性名
 * @param type      属性类型
 * @param desc      属性描述
 */
void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc)
{
    cJSON *prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", type);
    cJSON_AddStringToObject(prop, "description", desc);

    cJSON_AddItemToObject(props_obj, name, prop);
}
