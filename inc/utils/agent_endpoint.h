#ifndef __AGENT_UTILS_AGENT_ENDPOINT_H__
#define __AGENT_UTILS_AGENT_ENDPOINT_H__

#include <rtthread.h>

/*
 * 外部服务器地址。文件服务器、MCP 网关、管理控制台都在板子外面，
 * 地址有两个来源：
 *
 *   手动  三个地址都在 Kconfig 里填死，板端启动时不问控制台
 *   自动  只填控制台地址，板端启动时问一次，
 *         拿控制台报的文件服务器与 MCP 网关地址
 *
 * 自动模式拿不到控制台时回退到下面编译进固件的默认地址，
 * 所以控制台没开只是地址旧一点，不会让上传下载与 MCP 直接瘫掉。
 */

/*
 * 工程 .config 比包里的 Kconfig 旧时还没有这些选项，用默认地址兜底。
 * 自动获取模式下文件服务器与 MCP 的地址不在 menuconfig 里出现，
 * 固件编译时就是下面这个默认值，控制台连不上时当回退用。
 * 默认值按 QEMU 走 TAP 网桥接宿主机算，宿主机在共享网段上是 192.168.137.1。
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
