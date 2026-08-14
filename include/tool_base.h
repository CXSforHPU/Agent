#ifndef __AGENT_TOOL_BASE_H__
#define __AGENT_TOOL_BASE_H__

#include <rtthread.h>
#include "cJSON.h"
#include "utils.h"
#include  "chat.h"

#define LOG_TAG "Agent.tool_base"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>
/*
[
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
]
*/
typedef struct AgentToolRet
{
    /* data */
    rt_err_t ret;
    Message_t message;
}AgentToolRet,*AgentToolRet_t;


typedef struct AgentToolList
{
    cJSON* tool_obj;
    AgentToolRet ret;
    void (*execute_func)(cJSON* param,struct AgentToolList* node);
    struct AgentToolList* next;
}AgentToolList,AgentToolNode,*AgentToolNode_t;


rt_err_t AgentToolListCreate();
rt_err_t AppendTool(cJSON* tool_obj, void (*execute_func)(cJSON* param,struct AgentToolList* node));
cJSON* BulitToolsJson();
void FreeAgentToolNode(AgentToolNode_t node);
AgentToolNode_t SearchAgentToolNode(cJSON* tool_obj);
cJSON* GetAgentTools();

#endif /* __AGENT_TOOL_BASE_H__ */

