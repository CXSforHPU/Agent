#include "utils.h"

void print_reasoning(const char *text)
{

    LOG_I("Reasoning: \n%s", text);
    return;
}

void print_tool_call(const char *text)
{

    LOG_I("Tool Call: \n%s", text);
    return;
}

void print_context(const char *text)
{
    rt_kprintf("%s", text);
    return;
}

/*
@function: format_text
@description: 将发送文本格式化为JSON对象
@input: const char* text - 要格式化的文本
@output:
{
    "text": "要发送的文本",
    "type": "text"
}
*/
cJSON* format_text(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "type", "text");

    return root;
}

/* @function: create_content_array
 * @description: 创建一个空的内容数组
 * @return: cJSON* - 创建的内容数组对象
 * []
 */
cJSON* create_content_array()
{

    cJSON *content_array = cJSON_CreateArray();
    if (content_array == NULL)
    {
        return NULL;
    }
    return content_array;
}

/* @function: to_content
 * @description: 将文本添加到内容数组中
 * @param: cJSON* content_array - 内容数组对象
 * @param: const char* text - 要添加的文本
 * @return: cJSON* - 更新后的内容数组对象
 * [
 *   {
 *     "text": "要发送的文本",
 *     "type": "text"
 *   }
 * ]
 */
cJSON* to_content(cJSON *content_array, const char *text)
{

    cJSON_AddItemToArray(content_array, format_text(text));
    return content_array;
}



/* @function: create_tool_item
 * @description: 创建tool外层工具
 * @param: const char* func_name ：工具名称
 * @param: const char* desc  :工具描述
 * @return: cJSON* tool_root : 单个工具信息
    {
        'type': 'function',
        'function': {
            'name': 'get_weather',
            'description': 'Get the current weather for a given city.',
            'parameters': {
                'type': 'object',
                'properties': {
                    'city': {
                        'type': 'string',
                        'description': 'The name of the city to query weather for.',
                    },
                },
                'required': ['city'],
            },
        }
    }
 */
cJSON* create_tool_item(
    const char* func_name,
    const char* desc,
    cJSON* params_obj
)
{
    // 外层工具对象 { "type":"function", "function": {...} }
    cJSON* tool_root = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_root, "type", "function");

    // function 子对象
    cJSON* func_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(func_obj, "name", func_name);
    cJSON_AddStringToObject(func_obj, "description", desc);
    cJSON_AddItemToObject(func_obj, "parameters", params_obj);

    cJSON_AddItemToObject(tool_root, "function", func_obj);
    return tool_root;
}

/* @function: create_param_obj
 * @description: 创建parameters
 * @param: cJSON* props_arr: 参数列表
 * @param: cJSON* required_arr: 必要参数列表
 * @return: cJSON* params_root : 参数对象
    'parameters': {
        'type': 'object',
        'properties': {
            'city': {
                'type': 'string',
                'description': 'The name of the city to query weather for.',
            },
        },
        'required': ['city'],
    }
 */
cJSON* create_param_obj(
    cJSON* props_obj,
    cJSON* required_arr
)
{
    cJSON* param_root = cJSON_CreateObject();
    cJSON_AddStringToObject(param_root, "type", "object");
    cJSON_AddItemToObject(param_root, "properties", props_obj);
    cJSON_AddItemToObject(param_root, "required", required_arr);
    return param_root;
}

/* @function: create_property
 * @description: 创建单个参数
 * @param: cJSON* props_obj:参数字典
 * @param: const char* name：参数名称
 * @param: const char* type:参数类型
 * @param: const char* desc:参数描述
 * @return: cJSON* prop : 单个参数信息
{
    "city":{
        'type': 'string',
        'description': 'The name of the city to query weather for.',
    }
}
 */
void create_property(cJSON* props_obj,const char* name,const char* type, const char* desc)
{
    cJSON* prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", type);
    cJSON_AddStringToObject(prop, "description", desc);

    cJSON_AddItemToObject(props_obj,name,prop);
}