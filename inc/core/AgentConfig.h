#ifndef __AGENT_CONFIG_H__
#define __AGENT_CONFIG_H__

#include <rtthread.h>

#define AGENT_CFG_API_KEY_MAX     128
#define AGENT_CFG_MODEL_MAX       128
#define AGENT_CFG_BASE_URL_MAX    128

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
    char base_url[AGENT_CFG_BASE_URL_MAX];
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
 * @param  base_url   服务根地址，不含路径
 */
void agent_config_set(const char *api_key, const char *model_name, const char *base_url);

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
 * @brief  取当前协议的服务根地址
 * @return base URL 字符串，运行时未配置时返回当前协议在 Kconfig 里的默认值
 */
const char *get_dynamic_agent_base_url(void);

/*
 * @brief  拼出当前协议的完整请求地址
 * @note   在 base URL 后面接上协议路径：OpenAI 接 /chat/completions，
 *         Anthropic 接 /v1/messages。返回值指向函数内部的静态缓冲，
 *         同一时刻只能被一个请求线程使用。
 * @return 完整请求地址
 */
const char *get_dynamic_agent_request_url(void);

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
