#ifndef __WEBNET_UTIL_H__
#define __WEBNET_UTIL_H__

#include <rtthread.h>
#include <webnet.h>
#include <wn_session.h>
#include <cJSON.h>

/*
 * WebNet 通道的公共助手。AgentWebChannel 与 AgentA2aChannel 共用，
 * 所以不加 static。调用方负责 cJSON 对象的生命周期。
 */

/* @brief 安全取字符串，NULL 时返回空串 */
const char *agent_wn_safe_string(const char *value);

/* @brief 发一段已经拼好的 JSON 文本 */
void agent_wn_send_raw_json(struct webnet_session *session, int status_code,
                            const char *reason, const char *body);

/* @brief 发 cJSON 对象，内部负责序列化与释放文本 */
rt_err_t agent_wn_send_json_object(struct webnet_session *session, int status_code,
                                   const char *reason, cJSON *json);

/* @brief 发 {success, [field]: message} 形状的响应 */
void agent_wn_send_simple_json(struct webnet_session *session, int status_code,
                               const char *reason, rt_bool_t success,
                               const char *field, const char *message);

/* @brief 复制请求体，超长或为空返回 NULL，调用方负责 rt_free */
char *agent_wn_copy_request_body(struct webnet_session *session, rt_size_t max_size);

/* @brief 判断 JSON 值是否为真，兼容 bool、数字和 "true"/"1" 字符串 */
rt_bool_t agent_wn_json_value_is_true(const cJSON *item);

/* @brief 按 SSE 格式写一帧。event 为空时只写 data 行，正文里的换行会拆成多行 */
void agent_wn_send_sse_event(struct webnet_session *session, const char *event,
                             const char *data);

#endif /* __WEBNET_UTIL_H__ */
