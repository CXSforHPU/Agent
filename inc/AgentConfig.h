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

/*
 * @brief  取运行时配置
 * @return 配置结构体指针，调用者不释放
 */
agent_runtime_config_t *agent_config_get(void);

/*
 * @brief  写入运行时配置
 * @param  api_key    接口密钥
 * @param  model_name 模型名
 * @param  api_url    请求端点，完整 URL 含路径
 */
void agent_config_set(const char *api_key, const char *model_name, const char *api_url);

/*
 * @brief  取当前接口密钥
 * @return 密钥字符串，运行时未配置时返回 Kconfig 里的默认值
 */
const char *get_dynamic_agent_api_key(void);

/*
 * @brief  取当前模型名
 * @return 模型名字符串，运行时未配置时返回 Kconfig 里的默认值
 */
const char *get_dynamic_agent_model_name(void);

/*
 * @brief  取当前请求端点
 * @return URL 字符串，运行时未配置时返回 Kconfig 里的默认值
 */
const char *get_dynamic_agent_api_url(void);

/*
 * @brief  取当前协议
 * @return 协议枚举值
 */
agent_api_protocol_t get_dynamic_agent_api_protocol(void);

/*
 * @brief  切换协议
 * @param  protocol 目标协议
 */
void agent_config_set_protocol(agent_api_protocol_t protocol);

#endif /* __AGENT_CONFIG_H__ */
