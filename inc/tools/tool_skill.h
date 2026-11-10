#ifndef __AGENT_TOOL_SKILL_H__
#define __AGENT_TOOL_SKILL_H__

#include "tool_base.h"

/*
 * @brief  加载并执行一个技能
 * @param  args_obj 工具参数 JSON 对象，字段 name 为技能名
 * @param  node     工具节点，用于回填结果
 */
void tool_skill(cJSON *args_obj, AgentToolNode_t node);

#endif
