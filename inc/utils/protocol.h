#ifndef __AGENT_UTILS_PROTOCOL_H__
#define __AGENT_UTILS_PROTOCOL_H__

/*
 * LLM 协议统一入口。请求拼装与响应解析在 src/utils/protocol/ 下按协议分文件：
 * protocol_openai.c 走 /chat/completions，protocol_anthropic.c 走 /v1/messages。
 * 响应侧只关心少数字段，用 utils/json_reader.h 扫描，不建 cJSON 树。
 *
 * 上下文里的消息一律用规范格式（OpenAI 风格）：role + content，content 是字符串
 * 或 [{type, ...}] 数组，工具调用放在 tool_calls 里。各协议模块把规范视图直接
 * 写进请求缓冲，解析时再转回规范格式。
 */

#include <rtthread.h>
#include "cJSON.h"
#include "webclient.h"
#include "AgentConfig.h"
#include "core/context_msg.h"

#define AGENT_PROTOCOL_MAX_REASONING_LEN  PKG_AGENT_MAX_REASONING_LEN
#define AGENT_PROTOCOL_MAX_CONTENT_LEN    PKG_AGENT_MAX_CONTENT_LEN
#define AGENT_PROTOCOL_MAX_TOOL_ARG_LEN   PKG_AGENT_MAX_TOOL_ARG_LEN

/* 一轮回答里最多同时记录几个工具调用，超出部分丢弃并告警 */
#define AGENT_PROTOCOL_MAX_TOOL_CALLS     8
#define AGENT_PROTOCOL_TOOL_ID_LEN        64
#define AGENT_PROTOCOL_TOOL_NAME_LEN      48

/* 工具调用累积状态：id/name 定长，参数按需增长 */
typedef struct agent_protocol_tool
{
    int index;                                      /* 协议里的调用序号 */
    char id[AGENT_PROTOCOL_TOOL_ID_LEN];
    char name[AGENT_PROTOCOL_TOOL_NAME_LEN];
    char *args;                                     /* 参数 JSON，首次出现时分配 */
    int args_len;
    int args_cap;
} agent_protocol_tool_t;

/* 流式累积状态，两种协议共用 */
typedef struct agent_protocol_stream
{
    char *reasoning;              /* 思考过程文本 */
    int reasoning_len;
    char *content;                /* 回答正文 */
    int content_len;
    cJSON *tool_calls;            /* 归一化后的 tool_calls 数组 */
    agent_protocol_tool_t *tools; /* 按 index 累积的调用分片，首次出现工具调用时分配 */
    int tool_count;
    rt_bool_t is_answering;       /* 正文是否已经开始输出 */
    rt_bool_t content_lost;       /* 报文不完整或解析失败 */
    rt_bool_t saw_tool_call;      /* 本轮是否出现工具调用 */
    rt_bool_t natural_stop;       /* 正常收尾，不是被工具调用打断 */
    rt_bool_t truncated;          /* 输出被 max_tokens 截断（内容可用，只是没写完） */
    rt_bool_t stream_done;        /* 流已正常结束 */
    void (*on_reasoning)(const char *text);
    void (*on_tool_call)(const char *text);
    void (*on_context)(const char *text);
} agent_protocol_stream_t;

/* 协议操作表：一个协议一张表 */
typedef struct agent_protocol_ops
{
    const char *name;
    /* 把完整请求体直接写进 writer：model / messages / tools / max_tokens / stream */
    rt_err_t (*write_request)(agent_json_writer_t *writer, const agent_chat_src_t *src,
                              cJSON *tools, int max_tokens);
    /* 加鉴权与协议相关的请求头 */
    void (*add_headers)(struct webclient_session *session);
    /* 解析一条 SSE 事件的 JSON 文本，返回 RT_TRUE 表示流已结束 */
    rt_bool_t (*parse_event)(const char *json, int len, agent_protocol_stream_t *stream);
} agent_protocol_ops_t;

/*
 * @brief  取当前协议的操作表
 * @return 操作表指针，调用者不释放
 */
const agent_protocol_ops_t *agent_protocol_get_ops(void);

/*
 * @brief  取当前生效的协议
 * @return 协议枚举值
 */
agent_api_protocol_t agent_protocol_current(void);

/*
 * @brief  切换协议，同时改写运行时配置里的协议与端点
 * @param  protocol 目标协议
 */
void agent_protocol_switch(agent_api_protocol_t protocol);

/*
 * @brief  初始化流状态
 * @param  stream      流状态
 * @param  on_reasoning 思考过程分片回调
 * @param  on_tool_call 工具调用文本回调
 * @param  on_context   回答正文分片回调
 */
void agent_protocol_stream_init(agent_protocol_stream_t *stream,
                                 void (*on_reasoning)(const char *text),
                                 void (*on_tool_call)(const char *text),
                                 void (*on_context)(const char *text));

/*
 * @brief  释放流状态里累积的缓冲区
 * @param  stream 流状态
 */
void agent_protocol_stream_deinit(agent_protocol_stream_t *stream);

/*
 * @brief  累积一段思考过程
 * @param  stream 流状态
 * @param  seg    思考过程分片
 */
void agent_protocol_add_reasoning(agent_protocol_stream_t *stream, const char *seg);

/*
 * @brief  累积一段回答正文
 * @param  stream 流状态
 * @param  seg    正文分片
 */
void agent_protocol_add_content(agent_protocol_stream_t *stream, const char *seg);

/*
 * @brief  登记一个工具调用的 id 与函数名
 * @param  stream 流状态
 * @param  index  分片序号
 * @param  id     工具调用 id
 * @param  name   函数名
 */
void agent_protocol_tool_begin(agent_protocol_stream_t *stream, int index, const char *id, const char *name);

/*
 * @brief  追加一段工具调用参数
 * @param  stream   流状态
 * @param  index    分片序号
 * @param  fragment 参数分片
 */
void agent_protocol_tool_args(agent_protocol_stream_t *stream, int index, const char *fragment);

/*
 * @brief  把分片归一化成 tool_calls 数组，并回调 on_tool_call
 * @param  stream 流状态
 */
void agent_protocol_tool_finalize(agent_protocol_stream_t *stream);

/*
 * @brief  把一个字符串分片追加到定长缓冲
 * @param  buf       目标缓冲
 * @param  cap       缓冲容量
 * @param  len       已写入长度，成功追加后更新
 * @param  text      分片
 * @param  text_len  分片长度
 * @return RT_TRUE 放不下，发生了截断
 */
rt_bool_t agent_protocol_text_append(char *buf, int cap, int *len, const char *text, int text_len);

/*
 * @brief  累积工具调用的 id 或函数名，字符串收尾时登记到槽表
 * @param  stream    流状态
 * @param  index     分片序号
 * @param  buf       累积缓冲
 * @param  cap       缓冲容量
 * @param  len       已写入长度，成功后更新
 * @param  text      分片
 * @param  text_len  分片长度
 * @param  last      是否最后一个分片
 * @param  is_id     RT_TRUE 表示 id，RT_FALSE 表示函数名
 */
void agent_protocol_tool_field(agent_protocol_stream_t *stream, int index,
                               char *buf, int cap, int *len,
                               const char *text, int text_len,
                               rt_bool_t last, rt_bool_t is_id);

#endif /* __AGENT_UTILS_PROTOCOL_H__ */
