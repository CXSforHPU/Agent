#ifndef __AGENT_UTILS_H__
#define __AGENT_UTILS_H__
#include <rtthread.h>
#include "cJSON.h"

#define LOG_TAG "Agent.utils"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

cJSON* format_text(const char* text);
cJSON* create_content_array();
cJSON* to_content(cJSON* content_array, const char* text);

void print_reasoning(const char* text);
void print_tool_call(const char* text);
void print_context(const char* text);

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





cJSON* create_tool_item(
    const char* func_name,
    const char* desc,
    cJSON* params_obj
);
cJSON* create_param_obj(
    cJSON* props_obj,
    cJSON* required_arr
);

void create_property(cJSON* props_obj,const char* name,const char* type, const char* desc);

#endif // __AGENT_UTILS_H__