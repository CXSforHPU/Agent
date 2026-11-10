/*
 * MCP 工具接入：把远端 MCP 工具注册进 agent 工具表
 * 约束：远端工具一律带 mcp_ 前缀，与本地工具重名时跳过。
 */
#include "tool_func.h"

#ifdef PKG_AGENT_TOOL_MCP_ENABLE

#include "mcp_client.h"
#include "utils/tool_json.h"

#define LOG_TAG "Agent.tool_mcp"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define MCP_TOOL_PREFIX   "mcp_"
#define MCP_TOOL_NAME_MAX 64
#define MCP_DESC_MAX      1280

/*
 * @brief 把远端工具名转成注册名：加 mcp_ 前缀，非法字符换成下划线
 * @param remote 远端工具原名
 * @param out    输出缓冲区
 * @param outsz  缓冲区大小
 * @return RT_TRUE 可用，RT_FALSE 名字为空或超长
 * @note OpenAI 函数名只允许 [a-zA-Z0-9_-]，上限 64 字符
 */
static rt_bool_t make_registered_name(const char *remote, char *out, int outsz)
{
    int len = (int)rt_strlen(remote);
    int i;

    if (len == 0 || len + 4 > outsz - 1 || len + 4 > MCP_TOOL_NAME_MAX)
    {
        return RT_FALSE;
    }
    rt_strcpy(out, MCP_TOOL_PREFIX);
    for (i = 0; i < len; i++)
    {
        char c = remote[i];
        rt_bool_t valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[4 + i] = valid ? c : '_';
    }
    out[4 + len] = '\0';
    return RT_TRUE;
}

/*
 * @brief 组装注册用的工具描述
 * @param item    tools/list 里的远端工具项
 * @param remote  远端工具原名
 * @param out     输出缓冲区
 * @param outsz  缓冲区大小
 * @note 服务端没给描述时用工具名兜底；标了只读注解的补一句只读说明
 */
static void make_description(cJSON *item, const char *remote, char *out, int outsz)
{
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, "description");
    cJSON *annotations = cJSON_GetObjectItemCaseSensitive(item, "annotations");
    cJSON *read_only = annotations ? cJSON_GetObjectItemCaseSensitive(annotations, "readOnlyHint") : RT_NULL;

    if (cJSON_IsString(desc) && desc->valuestring[0])
    {
        rt_snprintf(out, outsz, "%s", desc->valuestring);
    }
    else
    {
        rt_snprintf(out, outsz, "MCP tool %s on the remote server.", remote);
    }
    if (cJSON_IsTrue(read_only))
    {
        size_t len = rt_strlen(out);
        if (len + 12 < (size_t)outsz)
        {
            rt_strcpy(out + len, " Read-only.");
        }
    }
}

/*
 * @brief 远端工具的统一执行入口
 * @param param 工具参数 JSON 对象
 * @param node  工具链表节点
 * @note 注册名从节点自身的 tool_obj 里读，去掉 mcp_ 前缀后转发 tools/call；
 *       结果文本写入 node->ret.messages，isError 映射为 -RT_ERROR
 */
static void mcp_tool_execute(cJSON *param, struct AgentToolList *node)
{
    cJSON *func;
    cJSON *name;
    char *text = RT_NULL;
    rt_err_t ret;

    if (!node)
    {
        return;
    }
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }
    node->ret.ret = -RT_ERROR;

    func = cJSON_GetObjectItemCaseSensitive(node->tool_obj, "function");
    name = func ? cJSON_GetObjectItemCaseSensitive(func, "name") : RT_NULL;
    if (!cJSON_IsString(name) ||
        rt_strncmp(name->valuestring, MCP_TOOL_PREFIX, rt_strlen(MCP_TOOL_PREFIX)) != 0)
    {
        LOG_E("tool_mcp: bad registered name");
        return;
    }

    ret = mcp_client_call(name->valuestring + rt_strlen(MCP_TOOL_PREFIX), param, &text);
    node->ret.messages = messages_create(1);
    if (node->ret.messages)
    {
        messages_append(node->ret.messages, TYPE_TEXT, text ? text : "MCP tool call failed.");
    }
    node->ret.ret = ret;
    rt_free(text);
}

/*
 * @brief 动态注册钩子：连接 MCP 服务端，把远端工具注册进 agent 工具表
 * @note 覆盖 tool_func.c 里的 rt_weak 空实现，在 init_tools() 里、模块注册
 *       之后、build_tools_json() 之前调用。连接失败只告警，不阻塞 agent
 *       启动；远端工具一律带 mcp_ 前缀，与本地工具重名时跳过。
 */
void agent_tool_register_dynamic(void)
{
    cJSON *tools;
    int total;
    int count;
    int registered = 0;
    int i;

    if (mcp_client_connect() != RT_EOK)
    {
        LOG_W("mcp: server unreachable, remote tools not registered");
        return;
    }
    tools = mcp_client_tools();
    total = cJSON_GetArraySize(tools);
    count = total > PKG_AGENT_MCP_MAX_TOOLS ? PKG_AGENT_MCP_MAX_TOOLS : total;
    if (total > count)
    {
        LOG_W("mcp: %d tools offered, only first %d registered", total, count);
    }

    for (i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(tools, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, "inputSchema");
        cJSON *params;
        cJSON *tool_obj;
        char reg_name[MCP_TOOL_NAME_MAX + 1];
        char desc[MCP_DESC_MAX];

        if (!cJSON_IsString(name) || !make_registered_name(name->valuestring, reg_name, sizeof(reg_name)))
        {
            LOG_W("mcp: skip tool with missing or overlong name");
            continue;
        }
        make_description(item, name->valuestring, desc, sizeof(desc));
        params = cJSON_IsObject(schema) ? cJSON_Duplicate(schema, cJSON_True)
                                        : create_param_obj(cJSON_CreateObject(), cJSON_CreateArray());
        tool_obj = create_tool_item(reg_name, desc, params);
        if (!tool_obj)
        {
            continue;
        }
        if (search_agent_tool_node(tool_obj))
        {
            LOG_W("mcp: tool %s already registered, skip", reg_name);
            cJSON_Delete(tool_obj);
            continue;
        }
        if (append_tool(tool_obj, mcp_tool_execute) != RT_EOK)
        {
            cJSON_Delete(tool_obj);
            continue;
        }
        registered++;
    }
    LOG_I("mcp: registered %d remote tools", registered);
}

/*
 * @brief 动态清理钩子：agent 退出时断开 MCP 客户端
 */
void agent_tools_cleanup_dynamic(void)
{
    mcp_client_disconnect();
}

#endif /* PKG_AGENT_TOOL_MCP_ENABLE */