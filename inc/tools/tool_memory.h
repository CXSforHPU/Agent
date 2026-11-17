#ifndef __AGENT_TOOL_MEMORY_H__
#define __AGENT_TOOL_MEMORY_H__

#include "tool_base.h"

void tool_memory_store(cJSON *args_obj, AgentToolNode_t node);
void tool_memory_search(cJSON *args_obj, AgentToolNode_t node);
void tool_memory_delete(cJSON *args_obj, AgentToolNode_t node);

#endif
