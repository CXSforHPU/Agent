#ifndef __AGENT_TOOL_MCP_CLIENT_H__
#define __AGENT_TOOL_MCP_CLIENT_H__

#include <rtthread.h>
#include "cJSON.h"

/*
 * MCP (Model Context Protocol) 客户端，走 Streamable HTTP 传输。
 * 服务端部署与启动脚本在包的 resource/file_server/mcp/ 目录：自带 HTTP 服务，或把 Windows
 * ODR 注册的 stdio 服务桥接成 HTTP。设备端只连一个服务端点，地址由
 * PKG_AGENT_MCP_SERVER_URL 配置。
 */

/*
 * @brief 连接服务端并完成握手
 * @return RT_EOK 成功，负值失败
 * @note 依次发送 initialize、notifications/initialized、tools/list，
 *       成功后 mcp_client_tools() 可取回远端工具表。已连接时直接返回
 *       RT_EOK。失败不保留半截状态，agent 启动流程可继续。
 */
rt_err_t mcp_client_connect(void);

/*
 * @brief 获取远端工具表
 * @return tools/list 返回的 tools 数组，由客户端持有，不得释放；未连接返回 NULL
 */
cJSON *mcp_client_tools(void);

/*
 * @brief 调用远端工具
 * @param name      远端工具名（tools/list 里的原名，不带 mcp_ 前缀）
 * @param arguments 参数 JSON 对象，只读，内部自行拷贝
 * @param out_text  输出文本，rt_malloc 分配，调用方负责 rt_free；失败时也
 *                  尽量给出可展示的原因
 * @return RT_EOK 成功，-RT_ERROR 服务端报错或 isError 为真，其余负值为传输失败
 * @note 未连接时先尝试重连一次。响应按 JSON-RPC 2.0 解析，兼容
 *       application/json 与 text/event-stream 两种回包。
 */
rt_err_t mcp_client_call(const char *name, cJSON *arguments, char **out_text);

/*
 * @brief 断开并释放客户端状态
 * @note 只清本地状态（会话 ID、工具表），不发送 HTTP DELETE：webclient
 *       没有 DELETE 动词，服务端在重启或收到新的 initialize 时回收旧会话。
 */
void mcp_client_disconnect(void);

#endif /* __AGENT_TOOL_MCP_CLIENT_H__ */