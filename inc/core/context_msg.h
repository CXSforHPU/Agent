#ifndef __AGENT_CONTEXT_MSG_H__
#define __AGENT_CONTEXT_MSG_H__

/*
 * 上下文消息的紧凑表示与请求来源描述。
 * 记录连续存放在 Context 的 arena 里，块内偏移一律相对记录起点，
 * 前缀丢弃只移动窗口，满时整体搬移也不需要修正任何偏移。
 *
 * 本头不做上下文管理的声明（见 core/context.h），只给协议层与测试提供
 * 只读视图和请求迭代器。
 */

#include <rtthread.h>
#include "utils/json_writer.h"

/* 角色类型枚举 */
typedef enum RoleType
{
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} RoleType;

/* 内容块类型 */
typedef enum agent_ctx_block_type
{
    AGENT_CTX_BLOCK_TEXT,        /* 文本，first 切片 */
    AGENT_CTX_BLOCK_IMAGE,       /* 图片 URL，first 切片 */
    AGENT_CTX_BLOCK_AUDIO,       /* 音频 URL，first 切片 */
    AGENT_CTX_BLOCK_VIDEO,       /* 视频 URL，first 切片 */
    AGENT_CTX_BLOCK_TOOL_CALL,   /* id / name / arguments 三个切片 */
    AGENT_CTX_BLOCK_TOOL_RESULT  /* tool_call_id 在 first，正文由 TEXT 块提供 */
} agent_ctx_block_type_t;

#define AGENT_CTX_FLAG_BLOCKS      0x01u  /* content 按块数组发送（多模态 user 消息） */
#define AGENT_CTX_FLAG_TOOL_CALLS  0x02u  /* assistant 消息带 tool_calls */

typedef struct agent_ctx_slice
{
    rt_uint32_t offset; /* 相对记录起点的偏移 */
    rt_uint32_t length; /* 字节数，不含结尾的 \0 */
} agent_ctx_slice_t;

typedef struct agent_ctx_block
{
    rt_uint8_t type;          /* agent_ctx_block_type_t */
    rt_uint8_t reserved[3];
    agent_ctx_slice_t first;  /* 文本 / URL / 工具 id / tool_call_id */
    agent_ctx_slice_t second; /* 工具名 */
    agent_ctx_slice_t third;  /* 工具参数 */
} agent_ctx_block_t;

typedef struct agent_ctx_message
{
    rt_uint8_t role;         /* RoleType */
    rt_uint8_t flags;        /* AGENT_CTX_FLAG_* */
    rt_uint16_t block_count;
    rt_uint32_t size;        /* 记录总字节数，含头、块与载荷 */
} agent_ctx_message_t;

/* 一条历史记录的只读视图 */
typedef struct agent_ctx_view
{
    const char *record;   /* 记录起点，块内偏移相对它 */
    rt_uint8_t role;
    rt_uint8_t flags;
    int block_count;
} agent_ctx_view_t;

/*
 * @brief  记录里的块数组
 * @param  view 记录视图
 * @return 块数组首地址
 */
static inline const agent_ctx_block_t *agent_ctx_view_blocks(const agent_ctx_view_t *view)
{
    return (const agent_ctx_block_t *)(view->record + sizeof(agent_ctx_message_t));
}

/*
 * @brief  把切片解析成 C 字符串
 * @param  view  记录视图
 * @param  slice 切片
 * @return 字符串指针
 */
static inline const char *agent_ctx_view_slice(const agent_ctx_view_t *view, const agent_ctx_slice_t *slice)
{
    return view->record + slice->offset;
}

/*
 * 请求来源：命中上下文的历史窗口，或者两段独立文本（压缩请求用）。
 * memory_text 非空时作为一条 user 消息放在系统提示词之后；
 * user_text 非空时追加在消息末尾，作为一条纯文本 user 消息。
 */
typedef struct agent_chat_src
{
    struct Context *ctx;
    int first;
    int count;                 /* < 0 表示到末尾 */
    const char *system_override;
    const char *memory_text;
    const char *user_text;
} agent_chat_src_t;

/* 规范消息迭代器：system -> 记忆 -> 摘要 -> 历史窗口 -> 追加文本 */
typedef struct agent_ctx_cursor
{
    const agent_chat_src_t *src;
    int stage;
    int index;
    int end;
    agent_ctx_view_t view;
} agent_ctx_cursor_t;

typedef struct agent_ctx_item
{
    int role;
    const char *text;       /* 字符串 content；块消息为 NULL */
    const char *record;     /* 块消息的记录起点；字符串消息为 NULL */
    agent_ctx_view_t *view; /* 块消息视图；字符串消息为 NULL */
} agent_ctx_item_t;

/*
 * @brief  初始化迭代器
 * @param  cursor 迭代器
 * @param  src    请求来源
 */
void agent_ctx_cursor_init(agent_ctx_cursor_t *cursor, const agent_chat_src_t *src);

/*
 * @brief  取下一条规范消息
 * @param  cursor 迭代器
 * @param  item   输出
 * @return RT_TRUE 还有消息
 */
rt_bool_t agent_ctx_cursor_next(agent_ctx_cursor_t *cursor, agent_ctx_item_t *item);

/*
 * @brief  把请求来源写成规范格式的消息数组
 * @param  writer 写入器
 * @param  src    请求来源
 */
void agent_context_write_messages(agent_json_writer_t *writer, const agent_chat_src_t *src);

/* 只读访问（协议层与回归用例用） */
int agent_context_message_count(const struct Context *self);
rt_bool_t agent_context_get_message(const struct Context *self, int index, agent_ctx_view_t *view);
const char *agent_context_system_prompt(const struct Context *self);
const char *agent_context_summary(const struct Context *self);
rt_bool_t agent_context_has_summary(const struct Context *self);

/*
 * @brief  最近一条 user 消息的纯文本，用作记忆检索的查询
 * @param  self 上下文
 * @return 文本指针；没有 user 消息返回 NULL
 */
const char *agent_context_last_user_text(const struct Context *self);

rt_size_t agent_context_arena_used(const struct Context *self);
rt_size_t agent_context_arena_capacity(const struct Context *self);

#endif // __AGENT_CONTEXT_MSG_H__
