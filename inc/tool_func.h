#ifndef TOOL_FUNC_H
#define TOOL_FUNC_H
#include "tool_base.h"
#include "tools.h"
#include "utils.h"

void init_tools();

/*
 * @brief 清理所有已注册工具并复位注册标志
 * @note 由 cleanup_agent 调用；再次进入对话时 init_tools() 会重新注册工具
 */
void agent_tools_cleanup(void);

#endif