#ifndef __AGENT_UTILS_PROTOCOL_H__
#define __AGENT_UTILS_PROTOCOL_H__

/*
 * LLM 协议统一入口
 *
 * 包内只跟这个头文件打交道，具体的请求拼装与响应解析在 src/utils/protocol/
 * 下按协议分模块实现：
 *   protocol_openai.c     OpenAI /chat/completions，Bearer 头，delta 式 SSE
 *   protocol_anthropic.c  Anthropic /v1/messages，x-api-key 头，事件式 SSE
 *
 * 上下文里保存的消息一律用规范格式（OpenAI 风格）：role + content，
 * content 是字符串或 [{type, ...}] 数组，工具调用放在 tool_calls 里。
 * 各协议模块负责把规范格式转成自己的报文格式，解析时再转回规范格式。
 */

#include <rtthread.h>
#include "cJSON.h"
#include "webclient.h"
#include "AgentConfig.h"

#define AGENT_PROTOCOL_MAX_REASONING_LEN  PKG_AGENT_MAX_REASONING_LEN
#define AGENT_PROTOCOL_MAX_CONTENT_LEN    PKG_AGENT_MAX_CONTENT_LEN
#define AGENT_PROTOCOL_MAX_TOOL_ARG_LEN   PKG_AGENT_MAX_TOOL_ARG_LEN

/* 流式累积状态，两种协议共用 */
typedef struct agent_protocol_stream
{
    char *reasoning;              /* 思考过程文本 */
    int reasoning_len;
    char *content;                /* 回答正文 */
    int content_len;
    cJSON *tool_calls;            /* 归一化后的 tool_calls 数组 */
    cJSON *tool_call_map;         /* 按 index 累积的分片 */
    rt_bool_t is_answering;       /* 正文是否已经开始输出 */
    rt_bool_t content_lost;       /* 报文不完整或解析失败 */
    rt_bool_t saw_tool_call;      /* 本轮是否出现工具调用 */
    rt_bool_t natural_stop;       /* 正常收尾，不是被工具调用打断 */
    rt_bool_t stream_done;        /* 流已正常结束 */
    void (*on_reasoning)(const char *text);
    void (*on_tool_call)(const char *text);
    void (*on_context)(const char *text);
} agent_protocol_stream_t;

/* 协议操作表：一个协议一张表 */
typedef struct agent_protocol_ops
{
    const char *name;
    /* 拼请求体：model / messages / tools / max_tokens / stream */
    rt_err_t (*build_request)(cJSON *payload, cJSON *messages, cJSON *tools, int max_tokens);
    /* 加鉴权与协议相关的请求头 */
    void (*add_headers)(struct webclient_session *session);
    /* 解析一条 SSE 事件，返回 RT_TRUE 表示流已结束 */
    rt_bool_t (*parse_event)(cJSON *json, agent_protocol_stream_t *stream);
    /* 规范格式的 content 数组转成本协议的 content 数组 */
    cJSON *(*format_content)(cJSON *content);
} agent_protocol_ops_t;

/* 取当前协议的操作表 */
const agent_protocol_ops_t *agent_protocol_get_ops(void);

/* 当前协议 */
agent_api_protocol_t agent_protocol_current(void);

/* 切换协议，同时改写运行时配置 */
void agent_protocol_switch(agent_api_protocol_t protocol);

/* 流状态初始化与释放 */
void agent_protocol_stream_init(agent_protocol_stream_t *stream,
                                 void (*on_reasoning)(const char *text),
                                 void (*on_tool_call)(const char *text),
                                 void (*on_context)(const char *text));
void agent_protocol_stream_deinit(agent_protocol_stream_t *stream);

/* 累积思考过程与正文 */
void agent_protocol_add_reasoning(agent_protocol_stream_t *stream, const char *seg);
void agent_protocol_add_content(agent_protocol_stream_t *stream, const char *seg);

/* 累积工具调用：先登记 id 与函数名，再追加参数分片 */
void agent_protocol_tool_begin(agent_protocol_stream_t *stream, int index, const char *id, const char *name);
void agent_protocol_tool_args(agent_protocol_stream_t *stream, int index, const char *fragment);

/* 把分片归一化成 tool_calls 数组，并回调 on_tool_call */
void agent_protocol_tool_finalize(agent_protocol_stream_t *stream);

#endif /* __AGENT_UTILS_PROTOCOL_H__ */
