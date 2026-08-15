#ifndef __AGENT_TOOL_MUL_H__
#define __AGENT_TOOL_MUL_H__
#include "tool_base.h"

/*
 * @brief 乘法工具执行函数
 * @param args_obj  参数 JSON 对象
 * @param node      工具链表节点（用于返回结果）
 */
void tool_mul(cJSON *args_obj, AgentToolNode_t node);

#endif // __AGENT_TOOL_MUL_H__