#ifndef __AGENT_TOOL_ADD_H__
#define __AGENT_TOOL_ADD_H__
#include "tool_base.h"

/*
 * @brief 加法工具执行函数
 * @param args_obj  参数 JSON 对象
 * @param node      工具链表节点（用于返回结果）
 */
void tool_add(cJSON *args_obj, AgentToolNode_t node);

#endif // __AGENT_TOOL_ADD_H__