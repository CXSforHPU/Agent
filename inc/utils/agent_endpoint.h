#ifndef __AGENT_UTILS_AGENT_ENDPOINT_H__
#define __AGENT_UTILS_AGENT_ENDPOINT_H__

#include <rtthread.h>

/*
 * 外部服务器地址。文件服务器与 MCP 网关都在板子外面，地址有两个来源：
 *
 *   手动  Kconfig 里写死，取不到也不换
 *   自动  启动时向管理控制台问一次，拿控制台报的文件服务器与 MCP 地址
 *
 * 自动模式拿不到控制台时回退到手动地址，所以控制台没开只是地址旧一点，
 * 不会让上传下载与 MCP 直接瘫掉。
 */

/* 工程 .config 比包里的 Kconfig 旧时还没有这些选项，用默认地址兜底 */
#ifndef PKG_AGENT_WEB_SERVER_URL
#define PKG_AGENT_WEB_SERVER_URL "http://10.0.2.2:8000"
#endif

#ifndef PKG_AGENT_MCP_SERVER_URL
#define PKG_AGENT_MCP_SERVER_URL "http://10.0.2.2:8765/mcp"
#endif

#ifndef PKG_AGENT_CONSOLE_URL
#define PKG_AGENT_CONSOLE_URL "http://10.0.2.2:8080"
#endif

#ifndef PKG_AGENT_CONSOLE_API
#define PKG_AGENT_CONSOLE_API "/api/ops/endpoints"
#endif

#ifndef PKG_AGENT_ENDPOINT_TIMEOUT_MS
#define PKG_AGENT_ENDPOINT_TIMEOUT_MS 3000
#endif

/* 运行时地址缓冲区与控制台响应体上限 */
#define AGENT_ENDPOINT_URL_MAX  128
#define AGENT_ENDPOINT_BODY_MAX 512

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

#endif /* __AGENT_UTILS_AGENT_ENDPOINT_H__ */
