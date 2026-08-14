#include "chat.h"
#include "context.h"
#include "MessageHub.h"
#include "tool_func.h"
#include "utils.h"
#include "AgentChannels.h"

#define LOG_TAG "Agent.AgentLoop"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>
static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;
static rt_thread_t g_main_agent_loop = RT_NULL;
static rt_bool_t g_agent_running = RT_FALSE;

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
static rt_thread_t g_file_op_thread = RT_NULL;
#endif

static void CleanupAgent(void);

static void InitAgent(){
    init_tools();
    g_message_hub = MessageHub_create();
    g_context = AgentContextCreate();

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
    g_file_op_thread = agent_file_op_init();
#endif

    AGENT_CHANNEL_IMPL(g_message_hub,g_context);
}


static void AgentLoop(Context_t g_context,
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
        resp = chat(g_context->message,tools,32*1024,on_reasoning,on_tool_call,on_context);
        if (resp == RT_NULL)
        {
            LOG_E("Chat request failed, terminating this case");
            break;
        }
        /* save assistant message */
        if (cJSON_GetArraySize(resp->tool_call) == 0)
        {
            // Store assistant text message
            g_context->append_assistant_message(g_context,resp);

            /* 复制内容到新消息，避免resp释放后悬空指针 */
            assistant_message = message_create(TYPE_TEXT, resp->context, 1);
            if (assistant_message)
            {
                g_message_hub->put_message(g_message_hub, assistant_message, g_message_hub->output_mailbox);
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
            int message_size = 0;

            // Parse tool arguments
            cJSON* args_json = cJSON_Parse(args_str);
            if (!args_json)
            {
                LOG_I("Tool %s parse arguments failed, skip execution\n", func_name);
                continue;
            }

            tool_node->execute_func(args_json,tool_node);

            message_size = tool_node->ret.message->size;
            for (int i = 0; i < message_size;i++)
            {
                Message_t item = &tool_node->ret.message[i];
                char* result_str = item->content;
                char* type = get_agent_content_type(item->message_type);
                LOG_I("[Local Tool %s Execution Result] type %s,result %s\n", func_name,type,result_str);
            }

            // Append tool response message

            g_context->append_tool_message(g_context,id_str,tool_node->ret.message);


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
    g_agent_running = RT_TRUE;
    while (g_agent_running)
    {
        Message_t message = g_message_hub->get_message(g_message_hub,g_message_hub->input_mailbox);
        /* RT_NULL 为 CleanupAgent 发出的唤醒信号，回 while 检查 g_agent_running */
        if (message == RT_NULL)
        {
            continue;
        }
        /* 消息由channel创建，context内部会复制内容，channel负责释放原消息 */
        g_context->append_user_message(g_context,message);

        AgentLoop(g_context,GetAgentTools(),print_reasoning,print_tool_call,print_context);
        /* 裁剪上下文，保留system prompt + 最近6条消息，防止无限膨胀 */
        if (g_context->trim_context)
        {
            g_context->trim_context(g_context, PKG_AGENT_MESSAGE_TRIM);
        }

    }
    /* 主循环退出后清理资源 */
    CleanupAgent();
}

static int MainLoop_entry(){
    if (g_main_agent_loop != RT_NULL)
    {
        LOG_W("Agent main loop already running");
        return 0;
    }
    g_main_agent_loop = rt_thread_create("AgentLoop",MainLoop,RT_NULL,10240,10,10);
    if (!g_main_agent_loop)
    {
        LOG_E("g_main_agent_loop thread create failed");
        return 1;
    }
    rt_thread_startup(g_main_agent_loop);
    return 0;
}

/* 清理线程不直接销毁，设置退出信号并唤醒 mailbox */
static void SignalAgentStop(void)
{
    g_agent_running = RT_FALSE;
    if (g_message_hub != RT_NULL && g_message_hub->input_mailbox != RT_NULL)
    {
        /* 发送一个RT_NULL哨兵唤醒主循环 */
        rt_mb_send(g_message_hub->input_mailbox, (rt_ubase_t)RT_NULL);
    }
}

static void CleanupAgent(void)
{
    if (g_message_hub)
    {
        MessageHub_destroy(g_message_hub);
        g_message_hub = RT_NULL;
    }
    if (g_context)
    {
        AgentContextDestroy(g_context);
        g_context = RT_NULL;
    }
    g_main_agent_loop = RT_NULL;
#ifdef PKG_AGENT_MULTIMODAL_ENABLE
    if(g_file_op_thread)
    {
        agent_file_op_deinit();
    }
    g_file_op_thread = RT_NULL;
#endif
}

static int CleanupAgent_entry(void)
{
    if (g_main_agent_loop == RT_NULL)
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
