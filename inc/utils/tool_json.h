#ifndef __AGENT_UTILS_TOOL_JSON_H__
#define __AGENT_UTILS_TOOL_JSON_H__

#include <rtthread.h>
#include "cJSON.h"

/* 工具节点类型定义在 tool_base.h，这里只用于函数指针参数 */
struct AgentToolList;

/*
 * 一个工具的静态描述。
 * describe 用 tool_desc 系列函数写发给模型的功能说明，params 用 create_property 逐个加
 * 参数、用 cJSON_AddItemToArray 标记必填项。注册时框架依次调用这两个构造函数，再交给
 * create_param_obj 和 create_tool_item 打包。
 * execute 是工具执行函数，签名与 tool_base.h 里的节点函数指针一致。
 */
struct agent_tool_def
{
    const char *name;
    void (*describe)(cJSON *func);
    void (*params)(cJSON *props, cJSON *required);
    void (*execute)(cJSON *args, struct AgentToolList *node);
};

/* 一个工具模块：模块名用于日志与自检，tools 是该模块提供的工具表 */
struct agent_tool_module_def
{
    const char *module;
    const struct agent_tool_def *tools;
    int tool_count;
};

/*
 * 模块自注册：在工具模块自己的 .c 文件末尾写一行
 *
 *     AGENT_TOOL_MODULE_EXPORT(pwm, pwm_tools)
 *
 * 宏生成一个模块描述符，并用 INIT_COMPONENT_EXPORT 在内核组件初始化阶段
 * 把它交给 agent_tool_module_add()。Kconfig 关掉某个模块，对应的 .c 不参与
 * 编译，模块表里自然没有它，中心文件不需要任何改动。
 */
#define AGENT_TOOL_MODULE_EXPORT(label, tools_)                                                      \
    static const struct agent_tool_module_def _agent_tool_module_##label =                           \
    {                                                                                                \
        #label, (tools_), (int)(sizeof(tools_) / sizeof((tools_)[0]))                                \
    };                                                                                               \
    static int _agent_tool_module_add_##label(void)                                                  \
    {                                                                                                \
        return agent_tool_module_add(&_agent_tool_module_##label);                                   \
    }                                                                                                \
    INIT_COMPONENT_EXPORT(_agent_tool_module_add_##label);

/*
 * @brief 创建工具项对象
 * @param func_name  函数名
 * @param desc       函数描述，传 RT_NULL 时先放一个空串，由 describe 回调补写
 * @param params_obj 参数对象
 * @return 工具项 cJSON 对象
 */
cJSON *create_tool_item(
    const char *func_name,
    const char *desc,
    cJSON *params_obj);

/*
 * @brief 创建参数对象 { type, properties, required }
 * @param props_obj    属性对象
 * @param required_arr 必填数组
 * @return 参数 cJSON 对象
 */
cJSON *create_param_obj(
    cJSON *props_obj,
    cJSON *required_arr);

/*
 * @brief 创建单个属性并添加到属性对象
 * @param props_obj 属性对象
 * @param name      属性名
 * @param type      属性类型
 * @param desc      属性描述
 */
void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc);

/*
 * @brief 写工具功能说明的主体
 * @param func 工具项里的 function 子对象
 * @param text 主体句子
 * @note 注册时 function 对象已带一个空的 description 字段，这里覆盖它，键顺序保持不变
 */
void tool_desc(cJSON *func, const char *text);

/*
 * @brief 追补一句说明，前面自动补一个空格
 * @param func 工具项里的 function 子对象
 * @param text 要追加的句子
 */
void tool_desc_more(cJSON *func, const char *text);

/* @brief 追加固定提示 "Read-only." */
void tool_desc_read_only(cJSON *func);

/* @brief 追加固定提示：写类操作要先拿到用户授权 */
void tool_desc_state_changing(cJSON *func);

/*
 * @brief 登记一个工具模块，由 AGENT_TOOL_MODULE_EXPORT 在内核组件初始化时调用
 * @param module 模块描述符，指向编译期常量，不需要释放
 * @return RT_EOK 成功，-RT_EINVAL 描述符不合法，-RT_EFULL 模块表已满
 */
rt_err_t agent_tool_module_add(const struct agent_tool_module_def *module);

/*
 * @brief 把模块表里全部工具注册到工具链表，由 init_tools() 调用
 */
void agent_tool_register_modules(void);

/* @brief 已登记的模块个数 */
int agent_tool_module_count(void);

/* @brief 第 index 个模块的名字，例如 "pwm"。越界返回 RT_NULL */
const char *agent_tool_module_name(int index);

/* @brief 第 index 个模块提供的工具个数。越界返回 0 */
int agent_tool_module_tools(int index);

/* @brief 第 module 个模块第 tool 个工具的名字。越界返回 RT_NULL */
const char *agent_tool_module_tool_name(int module, int tool);

#endif /* __AGENT_UTILS_TOOL_JSON_H__ */
