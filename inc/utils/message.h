#ifndef __AGENT_UTILS_MESSAGE_H__
#define __AGENT_UTILS_MESSAGE_H__

/*
 * @brief 打印思考过程（首个片段带 [thinking] 前缀，其后连续追加）
 */
void print_reasoning(const char *text);

/*
 * @brief 打印工具调用信息
 */
void print_tool_call(const char *text);

/*
 * @brief 打印回复内容，并复位思考前缀状态
 */
void print_context(const char *text);

/*
 * @brief 把一段文本作为用户消息注入 agent（等待 LLM 分析/总结/告警）
 * @param text 文本内容（非空）
 * @return RT_EOK 成功；RT_ERROR agent 未运行、消息中心未就绪或注入失败
 * @note  公共注入入口：工具/驱动/其它模块把外部事件主动交给 agent 时使用，
 *        不必自己拼 MessageHub。put 成功后消息所有权移交，调用方不得再释放；
 *        agent 未运行时返回 RT_ERROR，调用方需自行缓存或丢弃。
 *        例：mqtt_rx_thread 把订阅到的消息批量注入；其它工具可同样在自己的
 *        事件源（定时器、串口、传感器回调）里调用它。
 */
rt_err_t agent_inject_text(const char *text);

#endif // __AGENT_UTILS_MESSAGE_H__
