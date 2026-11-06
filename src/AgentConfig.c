#include "AgentConfig.h"

static agent_runtime_config_t g_agent_runtime_config =
{
    .api_key = PKG_AGENT_API_KEY,
    .model_name = PKG_AGENT_MODEL_NAME,
    .api_url = PKG_AGENT_API_URL,
    .is_configured = RT_TRUE
};

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
 * @brief 获取动态 API URL（优先返回运行时配置）
 * @return API URL 字符串
 */
const char *get_dynamic_agent_api_url(void)
{
    const agent_runtime_config_t *cfg = agent_config_get();
    if (cfg && cfg->is_configured && cfg->api_url[0])
    {
        return cfg->api_url;
    }
    return PKG_AGENT_API_URL;
}

/*
 * @brief 设置 agent 运行时配置
 * @param api_key    API Key
 * @param model_name 模型名
 * @param api_url    API URL
 */
void agent_config_set(const char *api_key, const char *model_name, const char *api_url)
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
    if (api_url && rt_strlen(api_url) < sizeof(g_agent_runtime_config.api_url))
    {
        rt_memset(g_agent_runtime_config.api_url, 0, sizeof(g_agent_runtime_config.api_url));
        rt_strncpy(g_agent_runtime_config.api_url, api_url, sizeof(g_agent_runtime_config.api_url));
        g_agent_runtime_config.api_url[sizeof(g_agent_runtime_config.api_url) - 1] = '\0';
    }
    g_agent_runtime_config.is_configured = RT_TRUE;
}