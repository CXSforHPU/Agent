#include "DebugChannel.h"

static MessageHub_t g_message_hub = RT_NULL;
static Context_t g_context = RT_NULL;


int AgentDebugChannel(MessageHub_t message_hub, Context_t context){
    if (g_message_hub == RT_NULL)
    {
        g_message_hub = message_hub;
    }
    if (g_context == RT_NULL)
    {
        g_context = context;
    }
}

// static int agent_send_message(int argv,char* argc[]){

// }