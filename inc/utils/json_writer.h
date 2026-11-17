#ifndef __AGENT_JSON_WRITER_H__
#define __AGENT_JSON_WRITER_H__

/*
 * JSON 直写模块：跳过 cJSON 中间树，把请求体直接写进字符缓冲。
 * buf 为 NULL 时只累计字节数，调用者先量一遍，再按实际大小分配并写第二遍。
 */

#include <rtthread.h>
#include "cJSON.h"

typedef struct agent_json_writer
{
    char *buf;          /* 输出缓冲，测量模式下为 NULL */
    rt_size_t cap;      /* 缓冲容量 */
    rt_size_t len;      /* 已写字节数（测量模式下也累计） */
    rt_bool_t overflow; /* 容量不足，输出不完整 */
} agent_json_writer_t;

/*
 * @brief  初始化 writer
 * @param  writer 写入器
 * @param  buf    输出缓冲，NULL 表示只测量长度
 * @param  cap    缓冲容量
 */
void agent_json_writer_init(agent_json_writer_t *writer, char *buf, rt_size_t cap);

/*
 * @brief  写入器是否仍然有效（未溢出）
 * @param  writer 写入器
 * @return RT_TRUE 没有溢出
 */
rt_bool_t agent_json_writer_ok(const agent_json_writer_t *writer);

/*
 * @brief  已写（或测量得到）的字节数
 * @param  writer 写入器
 * @return 字节数
 */
rt_size_t agent_json_writer_len(const agent_json_writer_t *writer);

/*
 * @brief  原样写入一段字节
 * @param  writer 写入器
 * @param  text   内容
 * @param  length 字节数
 */
void agent_json_write_raw(agent_json_writer_t *writer, const char *text, rt_size_t length);

/*
 * @brief  原样写入一个 C 字符串（用于标点与字面量）
 * @param  writer 写入器
 * @param  literal 内容
 */
void agent_json_write_literal(agent_json_writer_t *writer, const char *literal);

/*
 * @brief  写一个 JSON 字符串，含引号与转义，NULL 按空串处理
 * @param  writer 写入器
 * @param  text   文本
 */
void agent_json_write_string(agent_json_writer_t *writer, const char *text);

/*
 * @brief  写一个整数
 * @param  writer 写入器
 * @param  value  整数
 */
void agent_json_write_int(agent_json_writer_t *writer, int value);

/*
 * @brief  写 true / false
 * @param  writer 写入器
 * @param  value  布尔值
 */
void agent_json_write_bool(agent_json_writer_t *writer, rt_bool_t value);

/*
 * @brief  把一棵 cJSON 子树原样写进 writer
 * @param  writer 写入器
 * @param  item   子树，NULL 写成 null
 */
void agent_json_write_cjson(agent_json_writer_t *writer, const cJSON *item);

#endif // __AGENT_JSON_WRITER_H__
