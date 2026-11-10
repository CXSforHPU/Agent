#ifndef TOOL_FUNC_H
#define TOOL_FUNC_H
#include "tool_base.h"
#include "tools.h"
#include "utils.h"

void init_tools();

/*
 * @brief 动态工具注册钩子，默认空实现
 * @note 运行期才能确定工具集的模块（如 MCP 远端工具）提供强符号覆盖；
 *       调用时机在模块表注册之后、build_tools_json() 之前
 */
void agent_tool_register_dynamic(void);

/*
 * @brief 动态工具清理钩子，默认空实现，与注册钩子配对
 */
void agent_tools_cleanup_dynamic(void);

/*
 * @brief 清理所有已注册工具并复位注册标志
 * @note 由 cleanup_agent 调用；再次进入对话时 init_tools() 会重新注册工具
 */
void agent_tools_cleanup(void);

#endif