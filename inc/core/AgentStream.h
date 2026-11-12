#ifndef __AGENT_CORE_AGENT_STREAM_H__
#define __AGENT_CORE_AGENT_STREAM_H__

#include <rtthread.h>

/* 事件队列长度：一次对话里思考、工具、正文交替出现，8 条足够缓冲 */
#define AGENT_STREAM_QUEUE_SIZE 8
/* 单条事件的文本上限，超出部分截断 */
#define AGENT_STREAM_TEXT_MAX   256

/*
 * @brief 流式事件类型
 */
typedef enum
{
    AGENT_STREAM_REASONING = 0,  /* 思考过程分片 */
    AGENT_STREAM_TOOL_CALL,      /* 工具调用 */
    AGENT_STREAM_CONTENT,        /* 回复正文分片 */
    AGENT_STREAM_NOTICE          /* 通道侧提示，如请求失败 */
} agent_stream_kind_t;

/*
 * @brief 一条流式事件
 */
typedef struct
{
    agent_stream_kind_t kind;
    rt_size_t len;
    char text[AGENT_STREAM_TEXT_MAX];
} agent_stream_event_t;

/*
 * @brief 初始化事件队列
 * @note  在 agent 主循环初始化时调用一次
 */
void agent_stream_init(void);

/*
 * @brief 释放事件队列
 */
void agent_stream_deinit(void);

/*
 * @brief 投递一条流式事件
 * @param kind 事件类型
 * @param text 文本内容，超长自动截断
 * @note  队列满时覆盖最旧的一条，不阻塞发送方
 */
void agent_stream_emit(agent_stream_kind_t kind, const char *text);

/*
 * @brief 取出一条流式事件
 * @param event 事件输出
 * @return RT_TRUE 取到事件，RT_FALSE 队列为空
 */
rt_bool_t agent_stream_take(agent_stream_event_t *event);

/*
 * @brief 清空事件队列
 * @note  新任务开始前调用，避免上一轮的残留事件混进新一轮
 */
void agent_stream_clear(void);

/*
 * @brief 取事件类型的名字
 * @param kind 事件类型
 * @return 类型名字符串
 */
const char *agent_stream_kind_name(agent_stream_kind_t kind);

#endif /* __AGENT_CORE_AGENT_STREAM_H__ */
