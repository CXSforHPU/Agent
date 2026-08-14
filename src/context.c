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
多模态-单条信息
{
    "role": "user",
    "content": [
        {
            "type": "video_url",
            "video_url": {
                "url": "https://example.com/video.mp4",
                "detail": "high",
                "max_frames": 16,
                "fps": 1
            }
        },
        {
            "type": "image_url",
            "image_url": {"url": "https://example.com/thumbnail.jpg"}
        },
        {
            "type": "text",
            "text": "基于视频和缩略图，分析这个视频的主题和受众群体"
        }
    ]
}
{
    "role": "user",
    "content": [
        {
            "type": "image_url",
            "image_url": { 
                "url": "data:application/pdf;base64," + base64.b64encode(
                            open("xxx.pdf", "rb").read()).decode("utf-8")
                            }
        },
        {
            "type": "text",
            "text": "<image>\n<|grounding|>Convert the document to markdown. "
        }
    ]
}
*/
static rt_err_t __append_user_message(Context_t self, Message_t message)
{
    cJSON* user_message = RT_NULL;
    cJSON* content_obj = RT_NULL;
    rt_err_t rc = RT_EOK;

    if (!self || !message || !self->message) {
        return -RT_EINVAL;
    }

    user_message = cJSON_CreateObject();
    if (!user_message) {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(user_message, "role", Role(ROLE_USER));

    content_obj = to_content(message);

    cJSON_AddItemToObject(user_message,"content",content_obj);

    cJSON_AddItemToArray(self->message, user_message);

    user_message = RT_NULL;   // 所有权已转移
    content_obj = RT_NULL;
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
static rt_err_t __append_tool_message(Context_t self,const char* tool_call_id,Message_t message){
    cJSON* tool_msg = cJSON_CreateObject();
    cJSON* content_obj = RT_NULL;
    cJSON_AddStringToObject(tool_msg, "role", Role(ROLE_TOOL));
    cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id);

    content_obj = to_content(message);

    cJSON_AddItemToObject(tool_msg, "content", content_obj);
    cJSON_AddItemToArray(self->message, tool_msg);

    tool_msg = RT_NULL;
    content_obj = RT_NULL;
    return RT_EOK;
}

static rt_err_t __clear_message(Context_t self){
    if(self->message){
        cJSON_Delete(self->message);
        self->message = cJSON_CreateArray();
    }
    return RT_EOK;
}

/* 裁剪上下文：保留 system prompt + 最近 keep_items 条消息，
   防止 message 数组无限增长导致内存耗尽 */
static rt_err_t __trim_context(Context_t self, int keep_items)
{
    int total = cJSON_GetArraySize(self->message);
    if (total <= 1)  // 只有system prompt，无需裁剪
        return RT_EOK;

    /* 保留 system prompt（index 0），从 index 1 开始计算 */
    int history = total - 1;
    if (history <= keep_items)
        return RT_EOK;

    int remove_count = history - keep_items;
    /* 反复删除 index 1，后面的元素会自动前移 */
    for (int i = 0; i < remove_count; i++)
    {
        cJSON* item = cJSON_DetachItemFromArray(self->message, 1);
        cJSON_Delete(item);
    }
    LOG_D("Trimmed %d old messages, keeping last %d items", remove_count, keep_items);
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
    context->clear_message = __clear_message;
    context->trim_context = __trim_context;
    

    context->build_system_prompt(context);
    return context;
}

void AgentContextDestroy(Context_t context)
{
    if (context == RT_NULL)
    {
        return;
    }
    if (context->message)
    {
        cJSON_Delete(context->message);
        context->message = RT_NULL;
    }
    rt_free(context);
}
