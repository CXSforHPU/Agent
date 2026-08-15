#ifndef __AGENT_CHAT_H__
#define __AGENT_CHAT_H__

#include <rtthread.h>
#include "utils.h"
#include "webclient.h"
#include "AgentConfig.h"

/* 聊天响应结构体 */
typedef struct ChatResponse
{
    char *reasoning;      /* 思考过程文本 */
    char *context;        /* 回答内容文本 */
    cJSON *tool_call;     /* 工具调用数组 [list[Dict]] */
} ChatResponse, *ChatResponse_t;

/*
 * @brief 发送聊天请求并获取响应（流式）
 * @param messages   消息列表 cJSON 数组
 * @param tools      工具定义 cJSON 数组
 * @param max_tokens 最大 token 数
 * @param on_reasoning  思考过程回调
 * @param on_tool_call  工具调用回调
 * @param on_context    回复内容回调
 * @return ChatResponse_t 响应结构体，失败返回 NULL
 */
ChatResponse_t chat(
    cJSON *messages,
    cJSON *tools,
    int max_tokens,
    void (*on_reasoning)(const char *text),
    void (*on_tool_call)(const char *text),
    void (*on_context)(const char *text));

/*
 * @brief 释放聊天响应
 * @param resp 要释放的响应结构体
 */
void chat_response_free(ChatResponse_t resp);

#endif /* __AGENT_CHAT_H__ */