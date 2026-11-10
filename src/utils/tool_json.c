/*
 * 工具模块公共设施：工具项/参数 JSON 构造、描述拼装、模块注册表
 * 约束：模块表是静态数据，工具链表每次进入 agent 时重建，不持有堆内存。
 */
#include "utils/utils_internal.h"
#include "tool_base.h"

#define LOG_TAG "Agent.tool_json"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 模块表容量。设备类、运算、技能、MQTT 全开约 14 项，余量给 BSP 自备的模块。 */
#define AGENT_TOOL_MODULE_MAX 24

static const struct agent_tool_module_def *module_table[AGENT_TOOL_MODULE_MAX];
static int module_count = 0;

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
    cJSON_AddStringToObject(func_obj, "description", desc ? desc : "");
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

/* 固定提示句集中在这里，各模块的措辞不会各写一套 */
#define TOOL_NOTE_READ_ONLY    "Read-only."
#define TOOL_NOTE_STATE_CHANGE "State-changing: call only after the user authorized this peripheral."

/*
 * @brief 写入工具描述文本（已存在则覆盖）
 * @param func 工具项里的 function 子对象
 * @param text 描述文本
 */
static void tool_desc_set(cJSON *func, const char *text)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(func, "description");
    cJSON *fresh = cJSON_CreateString(text);

    if (!fresh)
    {
        return;
    }
    if (item)
    {
        /* 覆盖已有字段，键顺序仍是 name、description、parameters */
        cJSON_ReplaceItemInObjectCaseSensitive(func, "description", fresh);
        return;
    }
    cJSON_AddItemToObject(func, "description", fresh);
}

/*
 * @brief 写工具功能说明的主体
 * @param func 工具项里的 function 子对象
 * @param text 主体句子
 */
void tool_desc(cJSON *func, const char *text)
{
    if (!func || !text)
    {
        LOG_W("tool_desc called without a function object or text");
        return;
    }
    tool_desc_set(func, text);
}

/*
 * @brief 追补一句说明，前面自动补一个空格
 * @param func 工具项里的 function 子对象
 * @param text 要追加的句子
 */
void tool_desc_more(cJSON *func, const char *text)
{
    cJSON *item = RT_NULL;
    const char *old = RT_NULL;
    char *buffer = RT_NULL;
    rt_size_t size;

    if (!func || !text)
    {
        LOG_W("tool_desc_more called without a function object or text");
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(func, "description");
    old = (item && cJSON_IsString(item)) ? item->valuestring : RT_NULL;
    if (!old || !*old)
    {
        tool_desc_set(func, text);
        return;
    }

    size = rt_strlen(old) + rt_strlen(text) + 2;
    buffer = (char *)rt_malloc(size);
    if (!buffer)
    {
        return;
    }
    rt_snprintf(buffer, size, "%s %s", old, text);
    tool_desc_set(func, buffer);
    rt_free(buffer);
}

/*
 * @brief 给工具追加「只读」说明
 * @param func 工具项里的 function 子对象
 */
void tool_desc_read_only(cJSON *func)
{
    tool_desc_more(func, TOOL_NOTE_READ_ONLY);
}

/*
 * @brief 给工具追加「会改状态」说明
 * @param func 工具项里的 function 子对象
 */
void tool_desc_state_changing(cJSON *func)
{
    tool_desc_more(func, TOOL_NOTE_STATE_CHANGE);
}

/*
 * @brief 登记一个工具模块（由 AGENT_TOOL_MODULE_EXPORT 在内核组件初始化时调用）
 * @param module 模块描述符，指向编译期常量，不需要释放
 * @return RT_EOK 成功，-RT_EINVAL 描述符不合法，-RT_EFULL 模块表已满
 */
rt_err_t agent_tool_module_add(const struct agent_tool_module_def *module)
{
    int i;

    if (!module || !module->tools || module->tool_count <= 0)
    {
        return -RT_EINVAL;
    }
    for (i = 0; i < module_count; i++)
    {
        if (module_table[i] == module) return RT_EOK;
    }
    if (module_count >= AGENT_TOOL_MODULE_MAX)
    {
        LOG_E("Module table full, '%s' not registered", module->module);
        return -RT_EFULL;
    }
    module_table[module_count++] = module;
    return RT_EOK;
}

/*
 * @brief 按序号取已登记的工具模块
 * @param index 模块序号
 * @return 模块描述符，越界返回 RT_NULL
 */
static const struct agent_tool_module_def *module_get(int index)
{
    if (index < 0 || index >= module_count) return RT_NULL;
    return module_table[index];
}

/*
 * @brief 取已登记的工具模块数量
 * @return 模块数量
 */
int agent_tool_module_count(void)
{
    return module_count;
}

/*
 * @brief 取指定序号的工具模块名
 * @param index 模块序号
 * @return 模块名，越界返回 RT_NULL
 */
const char *agent_tool_module_name(int index)
{
    const struct agent_tool_module_def *module = module_get(index);

    return module ? module->module : RT_NULL;
}

/*
 * @brief 取指定序号的工具模块包含的工具数
 * @param index 模块序号
 * @return 工具数，越界返回 0
 */
int agent_tool_module_tools(int index)
{
    const struct agent_tool_module_def *module = module_get(index);

    return module ? module->tool_count : 0;
}

/*
 * @brief 取指定模块里第 tool 个工具的名字
 * @param module_index 模块序号
 * @param tool         模块内的工具序号
 * @return 工具名，越界返回 RT_NULL
 */
const char *agent_tool_module_tool_name(int module_index, int tool)
{
    const struct agent_tool_module_def *module = module_get(module_index);

    if (!module || tool < 0 || tool >= module->tool_count) return RT_NULL;
    return module->tools[tool].name;
}

/*
 * @brief 按模块的描述回调生成参数对象
 * @param params 模块的参数表回调（可为 NULL）
 * @return 参数 cJSON 对象，分配失败返回 RT_NULL
 */
static cJSON *tool_params_obj(void (*params)(cJSON *props, cJSON *required))
{
    cJSON *props = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();

    if (!props || !required)
    {
        cJSON_Delete(props);
        cJSON_Delete(required);
        return RT_NULL;
    }
    if (params)
    {
        params(props, required);
    }
    return create_param_obj(props, required);
}

/*
 * @brief 把模块表里的全部工具注册到工具链表
 * @note 由 init_tools() 调用；工具链表每次进入 agent 时重建，模块表是静态数据，
 *       因此反复进入不会重复注册，也不会残留堆内存
 */
void agent_tool_register_modules(void)
{
    int m, t;

    for (m = 0; m < module_count; m++)
    {
        const struct agent_tool_module_def *module = module_table[m];

        for (t = 0; t < module->tool_count; t++)
        {
            const struct agent_tool_def *def = &module->tools[t];
            cJSON *params = RT_NULL;
            cJSON *tool;

            if (!def->name || !def->execute)
            {
                LOG_E("Bad tool entry in module '%s'", module->module);
                continue;
            }
            params = tool_params_obj(def->params);
            if (!params) continue;

            tool = create_tool_item(def->name, RT_NULL, params);
            if (!tool)
            {
                cJSON_Delete(params);
                continue;
            }
            if (def->describe)
            {
                def->describe(cJSON_GetObjectItemCaseSensitive(tool, "function"));
            }
            else
            {
                LOG_W("Tool '%s' has no description", def->name);
            }
            if (append_tool(tool, def->execute) != RT_EOK)
            {
                cJSON_Delete(tool);
                LOG_E("Failed to register tool '%s'", def->name);
            }
        }
    }
}
