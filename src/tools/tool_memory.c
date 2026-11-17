/*
 * 记忆工具：模型自己维护核心记忆与回忆层。
 * 核心记忆每轮注入；回忆层按关键词检索，压缩丢掉的对话可在其中留底。
 */
#include "tool_memory.h"
#include "memory.h"

#ifdef PKG_AGENT_MEMORY_TOOLS_ENABLE

static void __reply(AgentToolNode_t node, rt_err_t result, const char *text)
{
    node->ret.messages = messages_create(1);
    if (node->ret.messages == RT_NULL)
    {
        node->ret.ret = -RT_ENOMEM;
        return;
    }
    if (messages_append(node->ret.messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
        node->ret.ret = -RT_ENOMEM;
        return;
    }
    node->ret.ret = result;
}

void tool_memory_store(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *text;
    cJSON *kind;
    char reply[256];
    int kind_value = AGENT_MEMORY_CORE;
    rt_err_t result;

    if (node == RT_NULL) return;
    messages_destroy(node->ret.messages);
    node->ret.messages = RT_NULL;
    node->ret.ret = -RT_ERROR;

    text = cJSON_GetObjectItemCaseSensitive(args_obj, "text");
    kind = cJSON_GetObjectItemCaseSensitive(args_obj, "kind");
    if (!cJSON_IsObject(args_obj) || !cJSON_IsString(text) ||
        (kind && !cJSON_IsString(kind)))
    {
        __reply(node, -RT_EINVAL, "Invalid arguments: expected {\"text\": string, \"kind\"?: \"core\"|\"recall\"}.");
        return;
    }
    if (kind)
    {
        kind_value = agent_memory_kind_parse(kind->valuestring);
        if (kind_value < 0)
        {
            __reply(node, -RT_EINVAL, "Unknown kind, use \"core\" or \"recall\".");
            return;
        }
    }

    result = agent_memory_store((agent_memory_kind_t)kind_value, text->valuestring);
    if (result == RT_EOK)
    {
        rt_snprintf(reply, sizeof(reply), "Stored to %s memory.",
                    agent_memory_kind_name((agent_memory_kind_t)kind_value));
    }
    else if (result == -RT_EINVAL)
    {
        rt_snprintf(reply, sizeof(reply), "Nothing to store: the text is empty.");
    }
    else
    {
        rt_snprintf(reply, sizeof(reply), "Store failed (%d).", result);
    }
    __reply(node, result, reply);
}

void tool_memory_search(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *query;
    cJSON *kind;
    cJSON *limit;
    char reply[512];
    int kind_value = -1;
    int limit_value = PKG_AGENT_MEMORY_TOPK;
    int listed;

    if (node == RT_NULL) return;
    messages_destroy(node->ret.messages);
    node->ret.messages = RT_NULL;
    node->ret.ret = -RT_ERROR;

    query = cJSON_GetObjectItemCaseSensitive(args_obj, "query");
    kind = cJSON_GetObjectItemCaseSensitive(args_obj, "kind");
    limit = cJSON_GetObjectItemCaseSensitive(args_obj, "limit");
    if (!cJSON_IsObject(args_obj) ||
        (query && !cJSON_IsString(query)) ||
        (kind && !cJSON_IsString(kind)) ||
        (limit && !cJSON_IsNumber(limit)))
    {
        __reply(node, -RT_EINVAL, "Invalid arguments: expected {\"query\"?: string, \"kind\"?: string, \"limit\"?: number}.");
        return;
    }
    if (kind)
    {
        kind_value = agent_memory_kind_parse(kind->valuestring);
        if (kind_value < 0)
        {
            __reply(node, -RT_EINVAL, "Unknown kind, use \"core\" or \"recall\".");
            return;
        }
    }
    if (limit && limit->valueint > 0)
    {
        limit_value = limit->valueint;
    }

    listed = agent_memory_query(kind_value, query ? query->valuestring : RT_NULL,
                               limit_value, reply, sizeof(reply));
    if (listed <= 0)
    {
        __reply(node, RT_EOK, "No matching memory.");
        return;
    }
    __reply(node, RT_EOK, reply);
}

void tool_memory_delete(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *id;
    char reply[96];
    rt_err_t result;

    if (node == RT_NULL) return;
    messages_destroy(node->ret.messages);
    node->ret.messages = RT_NULL;
    node->ret.ret = -RT_ERROR;

    id = cJSON_GetObjectItemCaseSensitive(args_obj, "id");
    if (!cJSON_IsObject(args_obj) || !cJSON_IsNumber(id))
    {
        __reply(node, -RT_EINVAL, "Invalid arguments: expected {\"id\": number}.");
        return;
    }

    result = agent_memory_delete(id->valueint);
    if (result == RT_EOK)
    {
        rt_snprintf(reply, sizeof(reply), "Deleted memory id %d.", id->valueint);
    }
    else
    {
        rt_snprintf(reply, sizeof(reply), "Memory id %d not found.", id->valueint);
    }
    __reply(node, result, reply);
}

static void store_desc(cJSON *func)
{
    tool_desc(func, "Save a durable fact, preference or decision to long-term memory.");
    tool_desc_more(func, "Core memory is injected into every request; use it for stable user "
        "preferences and constraints. Recall memory is searched by keyword; use it for "
        "episodic details you may need later.");
    tool_desc_more(func, "Store one self-contained sentence. Do not store secrets, passwords, "
        "tokens or transient device readings.");
}

static void store_params(cJSON *props, cJSON *required)
{
    create_property(props, "text", "string",
        "The fact or preference to remember, one self-contained sentence.");
    create_property(props, "kind", "string",
        "Where to store it: \"core\" (default, always visible) or \"recall\" (searched on demand).");
    cJSON_AddItemToArray(required, cJSON_CreateString("text"));
}

static void search_desc(cJSON *func)
{
    tool_desc(func, "Search long-term memory by keywords.");
    tool_desc_more(func, "Matches ASCII words and CJK characters, ranks by hit count and returns "
        "the newest items first. Without a query it lists the newest items.");
    tool_desc_more(func, "Use it when the conversation summary may have lost earlier details.");
}

static void search_params(cJSON *props, cJSON *required)
{
    create_property(props, "query", "string", "Keywords to look for; omit to list recent items.");
    create_property(props, "kind", "string", "\"core\" or \"recall\"; omit to search both.");
    create_property(props, "limit", "integer", "Maximum items to return, default 3.");
}

static void delete_desc(cJSON *func)
{
    tool_desc(func, "Delete one memory item by id.");
    tool_desc_more(func, "Search first to get the id shown in brackets. Deleting a wrong, stale "
        "or privacy-sensitive item keeps later requests accurate.");
}

static void delete_params(cJSON *props, cJSON *required)
{
    create_property(props, "id", "integer", "Memory id from a search result, for example [id=12 core].");
    cJSON_AddItemToArray(required, cJSON_CreateString("id"));
}

static const struct agent_tool_def memory_tools[] =
{
    {"memory_store", store_desc, store_params, tool_memory_store},
    {"memory_search", search_desc, search_params, tool_memory_search},
    {"memory_delete", delete_desc, delete_params, tool_memory_delete}
};

AGENT_TOOL_MODULE_EXPORT(memory, memory_tools)

#endif /* PKG_AGENT_MEMORY_TOOLS_ENABLE */
