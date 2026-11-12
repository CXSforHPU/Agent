#include "utils/utils_internal.h"

#define LOG_TAG "Agent.utils.endpoint"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 自动模式下向控制台要来的地址 */
static char g_file_server_url[AGENT_ENDPOINT_URL_MAX];
static char g_mcp_url[AGENT_ENDPOINT_URL_MAX];

const char *agent_endpoint_file_server(void)
{
    return g_file_server_url[0] ? g_file_server_url : PKG_AGENT_WEB_SERVER_URL;
}

const char *agent_endpoint_mcp(void)
{
    return g_mcp_url[0] ? g_mcp_url : PKG_AGENT_MCP_SERVER_URL;
}

#ifdef PKG_AGENT_EXT_SERVER_AUTO

/*
 * @brief 从控制台的响应里取一个字符串字段
 * @param root  响应根对象
 * @param key   data 下的字段名
 * @param out   输出缓冲区
 * @param size  缓冲区大小
 * @return RT_EOK 成功；负值表示字段缺失或放不下
 */
static rt_err_t endpoint_pick(cJSON *root, const char *key, char *out, size_t size)
{
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *item = data ? cJSON_GetObjectItemCaseSensitive(data, key) : RT_NULL;
    int n;

    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0')
    {
        LOG_W("[endpoint] no '%s' in the reply", key);
        return -RT_ERROR;
    }
    n = rt_snprintf(out, size, "%s", item->valuestring);
    if (n < 0 || n >= (int)size)
    {
        LOG_W("[endpoint] '%s' too long (%d chars)", key, n);
        return -RT_EINVAL;
    }
    return RT_EOK;
}

/*
 * @brief 向控制台问一次文件服务器与 MCP 网关的地址
 * @return RT_EOK 成功；负值失败
 */
static rt_err_t endpoint_fetch(void)
{
    struct webclient_session *session;
    char url[AGENT_ENDPOINT_URL_MAX + 64];
    char body[AGENT_ENDPOINT_BODY_MAX];
    char file_server[AGENT_ENDPOINT_URL_MAX];
    char mcp[AGENT_ENDPOINT_URL_MAX];
    cJSON *root;
    int status, total = 0, len, n;

    n = rt_snprintf(url, sizeof(url), "%s%s", PKG_AGENT_CONSOLE_URL, PKG_AGENT_CONSOLE_API);
    if (n < 0 || n >= (int)sizeof(url))
    {
        LOG_W("[endpoint] console url too long");
        return -RT_EINVAL;
    }

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
    {
        return -RT_ENOMEM;
    }

    LOG_I("[endpoint] GET %s", url);
    status = webclient_get(session, url);
    if (status != 200)
    {
        LOG_W("[endpoint] console returned %d", status);
        webclient_close(session);
        return -RT_ERROR;
    }
    /* 连接阶段的超时是 webclient 写死的，这里改的是读响应体的超时 */
    agent_webclient_set_timeout(session, PKG_AGENT_ENDPOINT_TIMEOUT_MS);

    while (total < (int)sizeof(body) - 1)
    {
        len = webclient_read(session, (unsigned char *)body + total,
                              sizeof(body) - 1 - total);
        if (len <= 0)
        {
            break;
        }
        total += len;
    }
    body[total] = '\0';
    webclient_close(session);

    root = cJSON_Parse(body);
    if (root == RT_NULL)
    {
        LOG_W("[endpoint] reply is not json: %s", body);
        return -RT_ERROR;
    }

    if (endpoint_pick(root, "fileServerUrl", file_server, sizeof(file_server)) != RT_EOK ||
        endpoint_pick(root, "mcpUrl", mcp, sizeof(mcp)) != RT_EOK)
    {
        cJSON_Delete(root);
        return -RT_ERROR;
    }
    cJSON_Delete(root);

    rt_strncpy(g_file_server_url, file_server, sizeof(g_file_server_url) - 1);
    g_file_server_url[sizeof(g_file_server_url) - 1] = '\0';
    rt_strncpy(g_mcp_url, mcp, sizeof(g_mcp_url) - 1);
    g_mcp_url[sizeof(g_mcp_url) - 1] = '\0';

    LOG_I("[endpoint] file server %s, mcp %s", g_file_server_url, g_mcp_url);
    return RT_EOK;
}

#endif /* PKG_AGENT_EXT_SERVER_AUTO */

void agent_endpoint_init(void)
{
#ifdef PKG_AGENT_EXT_SERVER_AUTO
    if (agent_endpoint_refresh() != RT_EOK)
    {
        LOG_W("[endpoint] use the fallback address in Kconfig");
    }
#endif
}

rt_err_t agent_endpoint_refresh(void)
{
#ifdef PKG_AGENT_EXT_SERVER_AUTO
    return endpoint_fetch();
#else
    return -RT_ENOSYS;
#endif
}

/*
 * @brief 打印与刷新外部服务器地址
 * @usage
 *   msh_agent_endpoint            打印当前地址
 *   msh_agent_endpoint refresh    重新向控制台问一次地址
 */
static int msh_agent_endpoint(int argc, char *argv[])
{
    rt_kprintf("mode       : %s\n",
#ifdef PKG_AGENT_EXT_SERVER_AUTO
               "auto (query the console at startup)"
#else
               "manual (from Kconfig)"
#endif
    );
    rt_kprintf("file server: %s\n", agent_endpoint_file_server());
    rt_kprintf("mcp        : %s\n", agent_endpoint_mcp());

    if (argc > 1 && (rt_strcmp(argv[1], "refresh") == 0 || rt_strcmp(argv[1], "reload") == 0))
    {
        rt_err_t rc = agent_endpoint_refresh();
        if (rc != RT_EOK)
        {
            rt_kprintf("refresh failed: %d\n", rc);
            return -1;
        }
        rt_kprintf("refreshed\n");
        rt_kprintf("file server: %s\n", agent_endpoint_file_server());
        rt_kprintf("mcp        : %s\n", agent_endpoint_mcp());
    }
    return 0;
}

MSH_CMD_EXPORT(msh_agent_endpoint, show or refresh the external server addresses);
