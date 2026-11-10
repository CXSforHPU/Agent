#include "AgentConfig.h"

#ifdef PKG_AGENT_API_PROTOCOL_ANTHROPIC
#define AGENT_CONFIG_DEFAULT_PROTOCOL   AGENT_API_PROTOCOL_ANTHROPIC
#else
#define AGENT_CONFIG_DEFAULT_PROTOCOL   AGENT_API_PROTOCOL_OPENAI
#endif

/* 协议路径：base URL 后面要接的那段 */
#define AGENT_PATH_OPENAI      "/chat/completions"
#define AGENT_PATH_ANTHROPIC   "/v1/messages"

#define AGENT_REQUEST_URL_MAX  (AGENT_CFG_BASE_URL_MAX + 32)

static agent_runtime_config_t g_agent_runtime_config =
{
    .api_key = PKG_AGENT_API_KEY,
    .model_name = PKG_AGENT_MODEL_NAME,
    .base_url = "",
    .protocol = AGENT_CONFIG_DEFAULT_PROTOCOL,
    .is_configured = RT_TRUE
};

/*
 * @brief 取协议路径
 * @param protocol 协议枚举
 * @return 路径字符串
 */
static const char *agent_config_path(agent_api_protocol_t protocol)
{
    return (protocol == AGENT_API_PROTOCOL_ANTHROPIC) ? AGENT_PATH_ANTHROPIC
                                                      : AGENT_PATH_OPENAI;
}

/*
 * @brief 获取 agent 运行时配置指针
 * @return 配置结构体指针
 */
agent_runtime_config_t *agent_config_get(void)
{
    return &g_agent_runtime_config;
}

/*
 * @brief 获取动态 API Key（优先返回运行时配置）
 * @return API Key 字符串
 */
const char *get_dynamic_agent_api_key(void)
{
    const agent_runtime_config_t *cfg = agent_config_get();
    if (cfg && cfg->is_configured && cfg->api_key[0])
    {
        return cfg->api_key;
    }
    return PKG_AGENT_API_KEY;
}

/*
 * @brief 获取动态模型名（优先返回运行时配置）
 * @return 模型名字符串
 */
const char *get_dynamic_agent_model_name(void)
{
    const agent_runtime_config_t *cfg = agent_config_get();
    if (cfg && cfg->is_configured && cfg->model_name[0])
    {
        return cfg->model_name;
    }
    return PKG_AGENT_MODEL_NAME;
}

/*
 * @brief 获取当前协议的服务根地址（优先返回运行时配置）
 * @return base URL 字符串
 */
const char *get_dynamic_agent_base_url(void)
{
    const agent_runtime_config_t *cfg = agent_config_get();
    if (cfg && cfg->is_configured && cfg->base_url[0])
    {
        return cfg->base_url;
    }
    return (get_dynamic_agent_api_protocol() == AGENT_API_PROTOCOL_ANTHROPIC)
           ? PKG_AGENT_API_BASE_URL_ANTHROPIC
           : PKG_AGENT_API_BASE_URL_OPENAI;
}

/*
 * @brief 拼出当前协议的完整请求地址
 * @note  返回值指向内部静态缓冲，同一时刻只供一个请求线程使用
 * @return 完整请求地址
 */
const char *get_dynamic_agent_request_url(void)
{
    static char request_url[AGENT_REQUEST_URL_MAX];
    const char *base = get_dynamic_agent_base_url();
    rt_size_t len;

    rt_strncpy(request_url, base, sizeof(request_url) - 1);
    request_url[sizeof(request_url) - 1] = '\0';

    /* 用户填了结尾的斜杠就去掉，避免拼出 //v1/messages */
    len = rt_strlen(request_url);
    while (len > 0 && request_url[len - 1] == '/')
    {
        request_url[--len] = '\0';
    }
    rt_snprintf(request_url + len, sizeof(request_url) - len, "%s",
                agent_config_path(get_dynamic_agent_api_protocol()));
    return request_url;
}

/*
 * @brief 获取当前使用的协议
 * @return 协议枚举
 */
agent_api_protocol_t get_dynamic_agent_api_protocol(void)
{
    const agent_runtime_config_t *cfg = agent_config_get();

    if (cfg && cfg->is_configured)
    {
        return cfg->protocol;
    }
    return AGENT_CONFIG_DEFAULT_PROTOCOL;
}

/*
 * @brief 切换当前使用的协议
 * @param protocol 协议枚举
 */
void agent_config_set_protocol(agent_api_protocol_t protocol)
{
    g_agent_runtime_config.protocol = protocol;
    g_agent_runtime_config.is_configured = RT_TRUE;
}

/*
 * @brief 设置 agent 运行时配置
 * @param api_key    API Key
 * @param model_name 模型名
 * @param base_url   服务根地址，不含路径
 */
void agent_config_set(const char *api_key, const char *model_name, const char *base_url)
{
    if (api_key && rt_strlen(api_key) < sizeof(g_agent_runtime_config.api_key))
    {
        rt_memset(g_agent_runtime_config.api_key, 0, sizeof(g_agent_runtime_config.api_key));
        rt_strncpy(g_agent_runtime_config.api_key, api_key, sizeof(g_agent_runtime_config.api_key));
        g_agent_runtime_config.api_key[sizeof(g_agent_runtime_config.api_key) - 1] = '\0';
    }
    if (model_name && rt_strlen(model_name) < sizeof(g_agent_runtime_config.model_name))
    {
        rt_memset(g_agent_runtime_config.model_name, 0, sizeof(g_agent_runtime_config.model_name));
        rt_strncpy(g_agent_runtime_config.model_name, model_name, sizeof(g_agent_runtime_config.model_name));
        g_agent_runtime_config.model_name[sizeof(g_agent_runtime_config.model_name) - 1] = '\0';
    }
    if (base_url && rt_strlen(base_url) < sizeof(g_agent_runtime_config.base_url))
    {
        rt_memset(g_agent_runtime_config.base_url, 0, sizeof(g_agent_runtime_config.base_url));
        rt_strncpy(g_agent_runtime_config.base_url, base_url, sizeof(g_agent_runtime_config.base_url));
        g_agent_runtime_config.base_url[sizeof(g_agent_runtime_config.base_url) - 1] = '\0';
    }
    g_agent_runtime_config.is_configured = RT_TRUE;
}
