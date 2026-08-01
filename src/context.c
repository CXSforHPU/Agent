#include "context.h"

static char AgentRole[4][16]={"system","user","assistant","tool"};


char* Role(RoleType type){
    return AgentRole[type];
}

/*
@function: __build_system_prompt
@description: 添加system_prompt
@input: 
        Context_t self:上下文管理器
@output:
self->payload
[
    {
        "role":"system",
        "content":"xxx"
    }
]
*/
static rt_err_t __build_system_prompt(Context_t self){

    cJSON* system_prompt = cJSON_CreateObject();
    cJSON_AddStringToObject(system_prompt,"role",Role(ROLE_SYSTEM));
    cJSON_AddStringToObject(system_prompt,"content",get_system_prompt());

    cJSON_AddItemToArray(self->message,system_prompt);

    return RT_EOK;
}
/*
@function: __append_user_message
@description: 添加用户信息
@input: 
        Context_t self:上下文管理器
        Message_t message:接受到的信息
@output:
self->payload
[
    {
        "role":"system",
        "content":"xxx"
    }，
    xxx,
    {
        "role":"user",
        "content":"xxx"
    }
]
*/
static rt_err_t __append_user_message(Context_t self,Message_t message){
    cJSON* user_message = cJSON_CreateObject();
    cJSON_AddStringToObject(user_message,"role",Role(ROLE_USER));
    cJSON_AddStringToObject(user_message,"content",rt_strdup(message->content));

    cJSON_AddItemToArray(self->message,user_message);

    // /* 销毁input message */
    // 由channel 方面删除
    // message_destroy(message);
    return RT_EOK;
}

/*
@function: __append_assistant_message
@description: 添加ai信息
@input: 
        Context_t self:上下文管理器
        ChatResponse_t resp：ai返回消息
@output:
self->payload
[
    {
        "role":"system",
        "content":"xxx"
    }，
    xxx,
    {
        "role":"user",
        "content":"xxx"
    }，
    {
        "role":"assistant",
        "content":"xxx"
    },
    {
        "role":"assistant",
        "tool_calls":{
            "id":"xxx",
            "type":"function",
            "function":{
                "name":"xxx",
                "arguments":"xxx"
            }
        }
    }
]
*/
static rt_err_t __append_assistant_message(Context_t self,ChatResponse_t resp){
    // No tool calls received: final answer obtained, exit loop
    if (cJSON_GetArraySize(resp->tool_call) == 0)
    {
        const char* ans = resp->context ? resp->context : "";
        // Store assistant text message
        cJSON* assist_message = cJSON_CreateObject();
        cJSON_AddStringToObject(assist_message, "role", Role(ROLE_ASSISTANT));
        cJSON_AddStringToObject(assist_message, "content", ans);
        cJSON_AddItemToArray(self->message, assist_message);

        return RT_EOK;
    }

    // Save assistant message with tool_calls to context
    cJSON* assist_tc = cJSON_CreateObject();
    cJSON_AddStringToObject(assist_tc, "role", Role(ROLE_ASSISTANT));
    cJSON_AddItemToObject(assist_tc, "tool_calls", cJSON_Duplicate(resp->tool_call, cJSON_True));
    cJSON_AddItemToArray(self->message, assist_tc);

    return RT_EOK;
}

/*
@function: __append_tool_message
@description: 添加工具返回信息
@input: 
        Context_t self:上下文管理器
        const char* tool_call_id:工具唯一标识
        const char* content：工具返回信息
@output:
self->payload
[
    {
        "role":"system",
        "content":"xxx"
    }，
    xxx,
    {
        "role":"user",
        "content":"xxx"
    }，
    {
        "role":"assistant",
        "content":"xxx"
    },
    {
        "role":"assistant",
        "tool_calls":{
            "id":"xxx",
            "type":"function",
            "function":{
                "name":"xxx",
                "arguments":"xxx"
            }
        }
    }
    {
        "role":"tool",
        "tool_call_id":"xxx"，
        "content":"xxx"
    }
]
*/
static rt_err_t __append_tool_message(Context_t self,const char* tool_call_id,const char* content){
    cJSON* tool_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_msg, "role", Role(ROLE_TOOL));
    cJSON_AddStringToObject(tool_msg, "tool_call_id",rt_strdup(tool_call_id));
    cJSON_AddStringToObject(tool_msg, "content", rt_strdup(content));
    cJSON_AddItemToArray(self->message, tool_msg);

    return RT_EOK;
}


Context_t AgentContextCreate(){
    Context_t context = (Context_t)rt_malloc(sizeof(Context));
    if (!context)
    {
        /* code */
        LOG_E("context malloc failed.");
        return RT_NULL;
    }
    context->message = cJSON_CreateArray();

    context->build_system_prompt = __build_system_prompt;
    context->append_user_message = __append_user_message;
    context->append_tool_message = __append_tool_message;
    context->append_assistant_message = __append_assistant_message;
    

    context->build_system_prompt(context);
    return context;
}
