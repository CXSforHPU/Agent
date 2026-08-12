#include "chat.h"
#include "context.h"
#include "MessageHub.h"
#include "tool_func.h"
#include "utils.h"
#include "AgentChannels.h"

#define LOG_TAG "Agent.AgentLoop"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>
static MessageHub_t message_hub = RT_NULL;
static Context_t context = RT_NULL;
static rt_thread_t main_agent_loop = RT_NULL;
static rt_bool_t agent_running = RT_FALSE;


static void CleanupAgent(void);

static void InitAgent(){
    init_tools();
    message_hub = MessageHub_create();
    context = AgentContextCreate();

    AGENT_CHANNEL_IMPL(message_hub,context);
}


static void AgentLoop(Context_t context,
    cJSON* tools,
    void (*on_reasoning)(const char* text),
    void (*on_tool_call)(const char* text),
    void (*on_context)(const char* text))
{
    const int loop_max = 10;
    int loop_cnt = 0;
    ChatResponse_t resp = RT_NULL;
    Message_t assistant_message = RT_NULL;

    while (loop_cnt < loop_max)
    {
        loop_cnt++;
        resp = chat(context->message,tools,32*1024,on_reasoning,on_tool_call,on_context);
        if (resp == RT_NULL)
        {
            LOG_E("Chat request failed, terminating this case");
            break;
        }
        /* save assistant message */
        if (cJSON_GetArraySize(resp->tool_call) == 0)
        {
            // Store assistant text message
            context->append_assistant_message(context,resp);

            /* 复制内容到新消息，避免resp释放后悬空指针 */
            assistant_message = message_create(ASSITANT, resp->context, RT_FALSE);
            if (assistant_message)
            {
                message_hub->put_message(message_hub, assistant_message, message_hub->output_mailbox);
            }
            chat_response_free(resp);
            resp = RT_NULL;
            break;
        }

        /* Execute all tool calls */
        int tool_count = cJSON_GetArraySize(resp->tool_call);
        LOG_I("\nDetected %d tool invocation(s)\n", tool_count);
        for (int t = 0; t < tool_count; t++)
        {

            cJSON* tc_item = cJSON_GetArrayItem(resp->tool_call, t);
            AgentToolNode_t tool_node =  SearchAgentToolNode(tc_item);

            if (!tc_item || !tool_node) continue;

            cJSON* tc_func = cJSON_GetObjectItemCaseSensitive(tc_item, "function");

            // Validate function node
            if (!tc_func || !cJSON_IsObject(tc_func))
            {
                LOG_I("Tool call %d missing function field, skip\n", t);
                continue;
            }

            cJSON* name_node = cJSON_GetObjectItemCaseSensitive(tc_func, "name");
            cJSON* args_node = cJSON_GetObjectItemCaseSensitive(tc_func, "arguments");
            cJSON* id_node = cJSON_GetObjectItem(tc_item,"id");
            if (!name_node || !cJSON_IsString(name_node) || !args_node || !cJSON_IsString(args_node))
            {
                LOG_I("Tool call %d invalid name/arguments, skip\n", t);
                continue;
            }

            const char* func_name = name_node->valuestring;
            const char* args_str = args_node->valuestring;
            const char* id_str = id_node->valuestring;

            // Parse tool arguments
            cJSON* args_json = cJSON_Parse(args_str);
            if (!args_json)
            {
                LOG_I("Tool %s parse arguments failed, skip execution\n", func_name);
                continue;
            }

            tool_node->execute_func(args_json,tool_node);

            LOG_I("[Local Tool %s Execution Result] %s\n", func_name, tool_node->ret.content);

            // Append tool response message

            context->append_tool_message(context,id_str,tool_node->ret.content);


            cJSON_Delete(args_json);
        }
        chat_response_free(resp);
        resp = RT_NULL;
    }

    if(loop_cnt >= loop_max){
        LOG_E("Maximum tool loop count %d reached, forced termination\n", loop_max);
        chat_response_free(resp);
    }
    return;
}



static void MainLoop(void* param){
    InitAgent();
    agent_running = RT_TRUE;
    while (agent_running)
    {
        Message_t message = message_hub->get_message(message_hub,message_hub->input_mailbox);
        /* RT_NULL 为 CleanupAgent 发出的唤醒信号，回 while 检查 agent_running */
        if (message == RT_NULL)
        {
            continue;
        }
        /* 消息由channel创建，context内部会cJSON_AddStringToObject复制内容，需释放原消息 */
        context->append_user_message(context,message);

        AgentLoop(context,GetAgentTools(),print_reasoning,print_tool_call,print_context);
        /* 裁剪上下文，保留system prompt + 最近6条消息，防止无限膨胀 */
        if (context->trim_context)
        {
            context->trim_context(context, 6);
        }

    }
    /* 主循环退出后清理资源 */
    CleanupAgent();
}

static int MainLoop_entry(){
    if (main_agent_loop != RT_NULL)
    {
        LOG_W("Agent main loop already running");
        return 0;
    }
    main_agent_loop = rt_thread_create("AgentLoop",MainLoop,RT_NULL,10240,10,10);
    if (!main_agent_loop)
    {
        LOG_E("main_agent_loop thread create failed");
        return 1;
    }
    rt_thread_startup(main_agent_loop);
    return 0;
}

/* 清理线程不直接销毁，设置退出信号并唤醒 mailbox */
static void SignalAgentStop(void)
{
    agent_running = RT_FALSE;
    if (message_hub != RT_NULL && message_hub->input_mailbox != RT_NULL)
    {
        /* 发送一个RT_NULL哨兵唤醒主循环 */
        rt_mb_send(message_hub->input_mailbox, (rt_ubase_t)RT_NULL);
    }
}

static void CleanupAgent(void)
{
    if (message_hub)
    {
        MessageHub_destroy(message_hub);
        message_hub = RT_NULL;
    }
    if (context)
    {
        AgentContextDestroy(context);
        context = RT_NULL;
    }
    main_agent_loop = RT_NULL;
}

static int CleanupAgent_entry(void)
{
    if (main_agent_loop == RT_NULL)
    {
        LOG_W("Agent not running, nothing to clean up");
        return 0;
    }
    LOG_I("Signaling agent to stop...");
    SignalAgentStop();
    return 0;
}

MSH_CMD_EXPORT(MainLoop_entry,MainLoop_entry);
MSH_CMD_EXPORT(CleanupAgent_entry,CleanupAgent_entry)
