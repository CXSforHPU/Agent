#ifndef __AGENT_CONFIG_H__
#define __AGENT_CONFIG_H__

#include <rtthread.h>

#define AGENT_CFG_API_KEY_MAX     128
#define AGENT_CFG_MODEL_MAX       128
#define AGENT_CFG_API_URL_MAX     128


#ifdef PKG_AGENT_API_KEY
#define API_KEY PKG_AGENT_API_KEY
#define API_URL PKG_AGENT_API_URL
#define API_MODEL PKG_AGENT_MODEL_NAME
#else
#define API_KEY ""
#define API_URL ""
#define API_MODEL ""
#endif


typedef struct agent_runtime_config
{
    char api_key[AGENT_CFG_API_KEY_MAX];
    char model_name[AGENT_CFG_MODEL_MAX];
    char api_url[AGENT_CFG_API_URL_MAX];
    rt_bool_t is_configured;
} rt_align(RT_ALIGN_SIZE) agent_runtime_config_t;

agent_runtime_config_t *agent_config_get(void);
void agent_config_set(const char *api_key, const char *model_name, const char *api_url);
const char *get_dynamic_agent_api_key(void);
const char *get_dynamic_agent_model_name(void);
const char *get_dynamic_agent_api_url(void);
#endif /* __AGENT_CONFIG_H__ */
