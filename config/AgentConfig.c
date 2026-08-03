#include "AgentConfig.h"

static agent_runtime_config_t g_agent_runtime_config =
{
    .api_key = PKG_AGENT_API_KEY,
    .model_name = PKG_AGENT_MODEL_NAME,
    .api_url = PKG_AGENT_API_URL,
    .is_configured = RT_TRUE
};

agent_runtime_config_t *agent_config_get(void)
{
    return &g_agent_runtime_config;
}

void agent_config_set(const char *api_key, const char *model_name, const char *api_url)
{
    if (api_key && rt_strlen(api_key) < sizeof(g_agent_runtime_config.api_key))
    {
        rt_strncpy(g_agent_runtime_config.api_key, api_key, sizeof(g_agent_runtime_config.api_key));
    }
    if (model_name && rt_strlen(model_name) < sizeof(g_agent_runtime_config.model_name))
    {
        rt_strncpy(g_agent_runtime_config.model_name, model_name, sizeof(g_agent_runtime_config.model_name));
    }
    if (api_url && rt_strlen(api_url) < sizeof(g_agent_runtime_config.api_url))
    {
        rt_strncpy(g_agent_runtime_config.api_url, api_url, sizeof(g_agent_runtime_config.api_url));
    }
    g_agent_runtime_config.is_configured = RT_TRUE;
}