#ifndef __AGENT_JSON_READER_H__
#define __AGENT_JSON_READER_H__

/*
 * JSON 读取模块：只扫描关心的字段，不建树、不分配内存。
 *
 * 用完整体的一行 JSON（例如一条 SSE 的 data 载荷）调用 agent_json_scan，
 * 传入一张字段路径表。扫描到路径匹配的值时回调：字符串按解码后的分片给出，
 * 对象/数组在起始位置给出类型，数字与字面量给出完整值。
 *
 * 路径用 '.' 分段，例如 "choices.0.delta.content"：
 *   - 普通分段与对象键名逐字节比较；
 *   - 全数字分段匹配数组下标；
 *   - "*" 匹配任意数组元素。
 * 键名里的转义序列不参与匹配，协议字段都是纯 ASCII，够用。
 */

#include <rtthread.h>

typedef enum
{
    AGENT_JSON_VALUE_OBJECT = 1,
    AGENT_JSON_VALUE_ARRAY,
    AGENT_JSON_VALUE_STRING,
    AGENT_JSON_VALUE_NUMBER,
    AGENT_JSON_VALUE_BOOL,
    AGENT_JSON_VALUE_NULL
} agent_json_value_type_t;

typedef struct agent_json_match
{
    agent_json_value_type_t type;
    const char *text;      /* STRING 分片，已解码且以 NUL 结尾；其他类型为 NULL */
    int len;               /* STRING 分片长度 */
    rt_bool_t last;        /* STRING 的最后一片；其他类型恒为 RT_TRUE */
    int number;            /* NUMBER 的整数值 */
    const int *index;      /* 路径里 "*" 命中的数组下标，按出现顺序 */
    int index_count;       /* 下标个数 */
} agent_json_match_t;

/*
 * @brief  字段命中回调
 * @param  user   调用方上下文
 * @param  watch  命中的路径在字段表里的下标
 * @param  match  值信息
 */
typedef void (*agent_json_on_match)(void *user, int watch, const agent_json_match_t *match);

/*
 * @brief  扫描一行 JSON，只回调字段表里登记的路径
 * @param  text         JSON 文本
 * @param  len          字节数
 * @param  watches      字段路径表
 * @param  watch_count  路径条数，最多 24 条
 * @param  on_match     命中回调
 * @param  user         回调上下文
 * @return RT_EOK 成功；-RT_EINVAL 报文不完整或语法错误
 */
rt_err_t agent_json_scan(const char *text, int len,
                         const char *const *watches, int watch_count,
                         agent_json_on_match on_match, void *user);

#endif // __AGENT_JSON_READER_H__
