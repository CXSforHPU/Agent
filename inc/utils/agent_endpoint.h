#ifndef __AGENT_UTILS_AGENT_ENDPOINT_H__
#define __AGENT_UTILS_AGENT_ENDPOINT_H__

#include <rtthread.h>

/*
 * File server, MCP and EMQX each choose manual or Web discovery.
 * Automatic endpoints share PKG_AGENT_CONSOLE_URL. Manual endpoints are
 * never overwritten by discovery. A failed refresh preserves the previous
 * automatic address, or falls back to the configured default on first boot.
 */

#ifndef PKG_AGENT_WEB_SERVER_URL
#define PKG_AGENT_WEB_SERVER_URL "http://192.168.137.1:8000"
#endif

#ifndef PKG_AGENT_MCP_SERVER_URL
#define PKG_AGENT_MCP_SERVER_URL "http://192.168.137.1:8765/mcp"
#endif

/* 自动获取模式下板端靠它找控制台，手动模式下板端不问控制台 */
#ifndef PKG_AGENT_CONSOLE_URL
#define PKG_AGENT_CONSOLE_URL "http://192.168.137.1:8080"
#endif

#ifndef PKG_AGENT_CONSOLE_API
#define PKG_AGENT_CONSOLE_API "/api/ops/endpoints"
#endif

#ifndef PKG_AGENT_ENDPOINT_TIMEOUT_MS
#define PKG_AGENT_ENDPOINT_TIMEOUT_MS 3000
#endif

/* Compatibility with firmware configuration generated before per-URL choices. */
#if defined(PKG_AGENT_EXT_SERVER_AUTO) && !defined(PKG_AGENT_ENDPOINT_INDEPENDENT)
#define PKG_AGENT_FILE_AUTO
#define PKG_AGENT_MCP_AUTO
#define PKG_AGENT_EMQX_AUTO
#endif

/* 运行时地址缓冲区与控制台响应体上限 */
#define AGENT_ENDPOINT_URL_MAX  128
#define AGENT_ENDPOINT_BODY_MAX 768

/*
 * @brief 初始化外部地址。自动模式下在这里向控制台问一次
 * @note  要在 MCP 客户端握手之前调用，否则握手用的是回退地址
 */
void agent_endpoint_init(void);

/*
 * @brief 重新向控制台问一次地址
 * @return RT_EOK 成功；负值失败，手动模式下返回 -RT_ENOSYS
 */
rt_err_t agent_endpoint_refresh(void);

/*
 * @brief 文件服务器根地址，不含路径
 */
const char *agent_endpoint_file_server(void);

/*
 * @brief MCP 网关地址，含 /mcp 路径
 */
const char *agent_endpoint_mcp(void);

/* Paho broker URI (tcp:// or ssl://). */
#ifndef PKG_AGENT_TOOL_MQTT_BROKER_URL
#define PKG_AGENT_TOOL_MQTT_BROKER_URL "tcp://broker.emqx.io:1883"
#endif
const char *agent_endpoint_emqx(void);

#endif /* __AGENT_UTILS_AGENT_ENDPOINT_H__ */
