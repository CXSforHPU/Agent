#ifndef __AGENT_UTILS_H__
#define __AGENT_UTILS_H__
#include "rtconfig.h"
#include <rtthread.h>
#include "cJSON.h"
#include "webclient.h"
#include "string.h"
#include "MessageHub.h"

#define LOG_TAG "Agent.utils"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

cJSON* format_text(const char* text);
cJSON* create_content_array();
cJSON* to_content(Message_t message);

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


/* 文件服务器 多模态启用*/
#ifdef PKG_AGENT_MULTIMODAL_ENABLE
typedef enum
{
    CMD_AGENT_FILE_UPLOAD,
    CMD_AGENT_FILE_DOWNLOAD,
    CMD_AGENT_FILE_EXIT,
    CMD_AGENT_FILE_URL
}file_op_type_t;

typedef struct
{
    file_op_type_t op;
    char path[64];
    char file_id[64];
}file_op_req;

// rt_mq_t get_agent_file_op_mq_input(void);
// rt_mq_t get_agent_file_op_mq_output(void);
rt_thread_t agent_file_op_init(void);
void agent_file_op_deinit(void);
char* agent_up_load(const char* path);
rt_err_t down_load(const char* path,const char* file_id);

#endif



#endif // __AGENT_UTILS_H__