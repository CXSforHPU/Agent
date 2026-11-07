#ifndef __AGENT_CONFIG_H__
#define __AGENT_CONFIG_H__

#include <rtthread.h>

#define AGENT_CFG_API_KEY_MAX     128
#define AGENT_CFG_MODEL_MAX       128
#define AGENT_CFG_API_URL_MAX     128

/* LLM 服务协议 */
typedef enum
{
    AGENT_API_PROTOCOL_OPENAI = 0,     /* POST /chat/completions，Bearer 头，delta 式 SSE */
    AGENT_API_PROTOCOL_ANTHROPIC = 1   /* POST /v1/messages，x-api-key 头，事件式 SSE */
} agent_api_protocol_t;

typedef struct agent_runtime_config
{
    char api_key[AGENT_CFG_API_KEY_MAX];
    char model_name[AGENT_CFG_MODEL_MAX];
    char api_url[AGENT_CFG_API_URL_MAX];
    agent_api_protocol_t protocol;
    rt_bool_t is_configured;
} rt_align(RT_ALIGN_SIZE) agent_runtime_config_t;

agent_runtime_config_t *agent_config_get(void);
void agent_config_set(const char *api_key, const char *model_name, const char *api_url);
const char *get_dynamic_agent_api_key(void);
const char *get_dynamic_agent_model_name(void);
const char *get_dynamic_agent_api_url(void);
agent_api_protocol_t get_dynamic_agent_api_protocol(void);
void agent_config_set_protocol(agent_api_protocol_t protocol);
#endif /* __AGENT_CONFIG_H__ */
