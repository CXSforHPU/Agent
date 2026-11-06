#include "tool_func.h"

static rt_err_t init_flag = RT_FALSE;

/*
 * @brief 初始化所有工具（遍历模块表，把各模块声明的工具注册到工具链表）
 * @note 每个工具模块在自己的 .c 文件里写一张描述符表，用 AGENT_TOOL_MODULE_EXPORT
 *       登记到模块表；Kconfig 或内核条件关掉的模块不参与编译，这里也就拿不到它。
 */
void init_tools(void)
{
    if (init_flag)
    {
        return;
    }

    agent_tool_list_create();
    agent_tool_register_modules();
    build_tools_json();
    init_flag = RT_TRUE;
}

/*
 * @brief 清理所有已注册工具并复位注册标志
 * @note 由 cleanup_agent 调用；再次进入对话时 init_tools() 会重新注册工具
 */
void agent_tools_cleanup(void)
{
    agent_tool_list_destroy();
    init_flag = RT_FALSE;
}

MSH_CMD_EXPORT(init_tools, init_tools);
