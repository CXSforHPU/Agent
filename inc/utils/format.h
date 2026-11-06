#ifndef __AGENT_UTILS_FORMAT_H__
#define __AGENT_UTILS_FORMAT_H__

/*
 * @brief 格式化文本为 cJSON 对象 { type, text }
 * @param text 文本内容
 * @return cJSON 对象指针
 */
cJSON *format_text(const char *text);

/*
 * @brief 创建空内容数组
 * @return cJSON 数组指针
 */
cJSON *create_content_array(void);

/*
 * @brief 将 Messages 转换为 API 要求的 content 格式
 * @param messages 消息数组
 * @return 非多模态返回 {content:xxx}，多模态返回 [{type, ...}]
 */
cJSON *to_content(Messages_t messages);

#endif // __AGENT_UTILS_FORMAT_H__
