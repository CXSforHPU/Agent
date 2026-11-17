#include "utils/utils_internal.h"

#define LOG_TAG "Agent.json"
#define LOG_LVL LOG_LVL_WARNING
#include <ulog.h>

/*
 * 流式 JSON 字段读取实现。
 *
 * 一条报文一次扫描：用显式的容器栈走 JSON 语法，遇到值先算出当前路径命中
 * 哪些字段，再把值交给回调。字符串边扫边解码、按小块交付，长字符串不需要
 * 额外缓冲；对象与数组只在起始处报类型；整条路径上的分配都来自调用方栈。
 */

#define AGENT_JSON_MAX_DEPTH      12
#define AGENT_JSON_MAX_WATCHES    24
#define AGENT_JSON_MAX_WILDCARDS  4
#define AGENT_JSON_CHUNK_BUF      64

typedef struct
{
    char kind;        /* '{' 或 '[' */
    int state;        /* 对象：key/colon/value/after；数组：value/after */
    rt_bool_t has_item; /* 已经收过成员，用来拒绝尾逗号 */
    const char *key;  /* 当前键名（原文，未解码） */
    int key_len;
    int index;        /* 当前数组下标 */
} agent_json_frame_t;

typedef struct
{
    int watch;
    int index_count;
    int index[AGENT_JSON_MAX_WILDCARDS];
} agent_json_hit_t;

typedef struct
{
    const char *p;
    const char *end;
    agent_json_frame_t stack[AGENT_JSON_MAX_DEPTH];
    int depth;
    const char *const *watches;
    int watch_count;
    agent_json_on_match on_match;
    void *user;
} agent_json_reader_t;

static void __skip_ws(agent_json_reader_t *r)
{
    while (r->p < r->end)
    {
        char c = *r->p;

        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        r->p++;
    }
}

static int __hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static rt_bool_t __parse_hex4(const char *p, unsigned int *value)
{
    unsigned int v = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        int nibble = __hex_value(p[i]);

        if (nibble < 0)
        {
            return RT_FALSE;
        }
        v = (v << 4) | (unsigned int)nibble;
    }
    *value = v;

    return RT_TRUE;
}

/* 按 cJSON 的规则编码：非法码点与孤立代理项返回 0 */
static int __utf8_encode(unsigned int codepoint, char *out)
{
    if (codepoint < 0x80)
    {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000)
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }

    return 0;
}

/*
 * @brief 解码一个转义序列
 * @param pp      指向反斜杠，成功后移到序列之后
 * @param end     报文末尾
 * @param out     解码输出，至少 4 字节
 * @return 输出字节数，非法或截断返回 -1
 */
static int __decode_escape(const char **pp, const char *end, char *out)
{
    const char *p = *pp;
    unsigned int code;
    int len;

    if (p + 1 >= end)
    {
        return -1;
    }

    switch (p[1])
    {
    case 'b': out[0] = '\b'; *pp = p + 2; return 1;
    case 'f': out[0] = '\f'; *pp = p + 2; return 1;
    case 'n': out[0] = '\n'; *pp = p + 2; return 1;
    case 'r': out[0] = '\r'; *pp = p + 2; return 1;
    case 't': out[0] = '\t'; *pp = p + 2; return 1;
    case '"':
    case '\\':
    case '/':
        out[0] = p[1];
        *pp = p + 2;
        return 1;
    case 'u':
        if (p + 6 > end || !__parse_hex4(p + 2, &code))
        {
            return -1;
        }
        if (code >= 0xDC00 && code <= 0xDFFF)
        {
            return -1;
        }
        if (code >= 0xD800 && code <= 0xDBFF)
        {
            unsigned int low;

            if (p + 12 > end || p[6] != '\\' || p[7] != 'u' ||
                !__parse_hex4(p + 8, &low) || low < 0xDC00 || low > 0xDFFF)
            {
                return -1;
            }
            code = 0x10000 + (((code & 0x3FF) << 10) | (low & 0x3FF));
            *pp = p + 12;
        }
        else
        {
            *pp = p + 6;
        }
        len = __utf8_encode(code, out);
        return len > 0 ? len : -1;
    default:
        return -1;
    }
}

static rt_bool_t __segment_match(const agent_json_reader_t *r, int level,
                                 const char *token, int token_len)
{
    const agent_json_frame_t *frame = &r->stack[level];

    if (frame->kind == '[')
    {
        int want = 0;
        int i;

        if (token_len == 1 && token[0] == '*')
        {
            return RT_TRUE;
        }
        if (token_len <= 0)
        {
            return RT_FALSE;
        }
        for (i = 0; i < token_len; i++)
        {
            if (token[i] < '0' || token[i] > '9')
            {
                return RT_FALSE;
            }
            want = want * 10 + (token[i] - '0');
        }
        return want == frame->index;
    }

    return token_len == frame->key_len &&
           rt_memcmp(token, frame->key, (rt_size_t)token_len) == 0;
}

/*
 * @brief 判断一个路径是否命中当前值，命中时顺带收集 * 对应的下标
 * @param r         读取器，stack 已包含当前值的所有上级容器
 * @param path      字段路径
 * @param hit       命中信息
 * @return RT_TRUE 命中
 */
static rt_bool_t __path_match(const agent_json_reader_t *r, const char *path,
                              agent_json_hit_t *hit)
{
    int level = 0;
    int count = 0;
    const char *p = path;

    for (;;)
    {
        const char *dot;
        int token_len;

        if (level >= r->depth || *p == '\0')
        {
            if (level != r->depth || *p != '\0')
            {
                return RT_FALSE;
            }
            hit->index_count = count;
            return RT_TRUE;
        }

        dot = p;
        while (*dot && *dot != '.')
        {
            dot++;
        }
        token_len = (int)(dot - p);

        if (!__segment_match(r, level, p, token_len))
        {
            return RT_FALSE;
        }
        if (token_len == 1 && p[0] == '*')
        {
            if (count >= AGENT_JSON_MAX_WILDCARDS)
            {
                return RT_FALSE;
            }
            hit->index[count++] = r->stack[level].index;
        }

        level++;
        p = (*dot == '.') ? dot + 1 : dot;
    }
}

static int __collect_hits(agent_json_reader_t *r, agent_json_hit_t *hits)
{
    int count = 0;
    int i;

    for (i = 0; i < r->watch_count; i++)
    {
        hits[count].watch = i;
        hits[count].index_count = 0;
        if (__path_match(r, r->watches[i], &hits[count]))
        {
            count++;
        }
    }

    return count;
}

static void __emit(agent_json_reader_t *r, const agent_json_hit_t *hits, int count,
                   agent_json_value_type_t type, const char *text, int len,
                   rt_bool_t last, int number)
{
    agent_json_match_t match;
    int i;

    if (!r->on_match || count <= 0)
    {
        return;
    }

    match.type = type;
    match.text = text;
    match.len = len;
    match.last = last;
    match.number = number;

    for (i = 0; i < count; i++)
    {
        match.index = hits[i].index;
        match.index_count = hits[i].index_count;
        r->on_match(r->user, hits[i].watch, &match);
    }
}

/*
 * @brief 解析一个字符串值，已解码的分片逐个交给命中字段
 * @param r         读取器，p 指向开引号
 * @param hits      命中的字段
 * @param hit_count 命中条数
 * @return RT_EOK 成功
 */
static rt_err_t __parse_string(agent_json_reader_t *r,
                               const agent_json_hit_t *hits, int hit_count)
{
    char chunk[AGENT_JSON_CHUNK_BUF + 1];
    int chunk_len = 0;

    if (r->p >= r->end || *r->p != '"')
    {
        return -RT_EINVAL;
    }
    r->p++;

    for (;;)
    {
        char decoded[4];
        int decoded_len;

        if (r->p >= r->end)
        {
            return -RT_EINVAL;
        }

        if (*r->p == '"')
        {
            r->p++;
            break;
        }

        if (*r->p == '\\')
        {
            decoded_len = __decode_escape(&r->p, r->end, decoded);
            if (decoded_len < 0)
            {
                return -RT_EINVAL;
            }
        }
        else
        {
            decoded[0] = *r->p++;
            decoded_len = 1;
        }

        if (chunk_len + decoded_len > AGENT_JSON_CHUNK_BUF)
        {
            chunk[chunk_len] = '\0';
            __emit(r, hits, hit_count, AGENT_JSON_VALUE_STRING, chunk, chunk_len, RT_FALSE, 0);
            chunk_len = 0;
        }
        rt_memcpy(chunk + chunk_len, decoded, (rt_size_t)decoded_len);
        chunk_len += decoded_len;
    }

    chunk[chunk_len] = '\0';
    __emit(r, hits, hit_count, AGENT_JSON_VALUE_STRING, chunk, chunk_len, RT_TRUE, 0);

    return RT_EOK;
}

/*
 * @brief 读一个键名，原文保存，转义序列只做合法性检查
 * @param r     读取器，p 指向开引号
 * @param frame 当前对象帧
 * @return RT_EOK 成功
 */
static rt_err_t __parse_key(agent_json_reader_t *r, agent_json_frame_t *frame)
{
    const char *start;
    char ditch[4];

    if (r->p >= r->end || *r->p != '"')
    {
        return -RT_EINVAL;
    }

    start = ++r->p;
    while (r->p < r->end && *r->p != '"')
    {
        if (*r->p == '\\')
        {
            if (__decode_escape(&r->p, r->end, ditch) < 0)
            {
                return -RT_EINVAL;
            }
        }
        else
        {
            r->p++;
        }
    }
    if (r->p >= r->end)
    {
        return -RT_EINVAL;
    }

    frame->key = start;
    frame->key_len = (int)(r->p - start);
    r->p++;

    return RT_EOK;
}

static rt_err_t __parse_number(agent_json_reader_t *r, int *value)
{
    int sign = 1;
    long long v = 0;
    rt_bool_t has_digit = RT_FALSE;

    if (r->p < r->end && *r->p == '-')
    {
        sign = -1;
        r->p++;
    }

    while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
    {
        if (v < 100000000LL)
        {
            v = v * 10 + (*r->p - '0');
        }
        has_digit = RT_TRUE;
        r->p++;
    }
    if (!has_digit)
    {
        return -RT_EINVAL;
    }

    if (r->p < r->end && *r->p == '.')
    {
        r->p++;
        if (r->p >= r->end || *r->p < '0' || *r->p > '9')
        {
            return -RT_EINVAL;
        }
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
        {
            r->p++;
        }
    }

    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E'))
    {
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-'))
        {
            r->p++;
        }
        if (r->p >= r->end || *r->p < '0' || *r->p > '9')
        {
            return -RT_EINVAL;
        }
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
        {
            r->p++;
        }
    }

    *value = (int)(sign * v);

    return RT_EOK;
}

/*
 * @brief 解析一个值；容器在起始处上报类型，随后压栈交给主循环
 * @param r 读取器
 * @return RT_EOK 成功
 */
static rt_err_t __parse_value(agent_json_reader_t *r)
{
    agent_json_hit_t hits[AGENT_JSON_MAX_WATCHES];
    int hit_count = __collect_hits(r, hits);
    char c;

    __skip_ws(r);
    if (r->p >= r->end)
    {
        return -RT_EINVAL;
    }

    c = *r->p;

    if (c == '{' || c == '[')
    {
        agent_json_frame_t *frame;

        __emit(r, hits, hit_count,
               (c == '{') ? AGENT_JSON_VALUE_OBJECT : AGENT_JSON_VALUE_ARRAY,
               RT_NULL, 0, RT_TRUE, 0);

        if (r->depth >= AGENT_JSON_MAX_DEPTH)
        {
            return -RT_EINVAL;
        }
        frame = &r->stack[r->depth++];
        frame->kind = c;
        frame->state = 0;
        frame->has_item = RT_FALSE;
        frame->key = RT_NULL;
        frame->key_len = 0;
        frame->index = 0;
        r->p++;

        return RT_EOK;
    }

    if (c == '"')
    {
        return __parse_string(r, hits, hit_count);
    }

    if (c == 't' && r->end - r->p >= 4 && rt_memcmp(r->p, "true", 4) == 0)
    {
        r->p += 4;
        __emit(r, hits, hit_count, AGENT_JSON_VALUE_BOOL, RT_NULL, 0, RT_TRUE, 1);
        return RT_EOK;
    }

    if (c == 'f' && r->end - r->p >= 5 && rt_memcmp(r->p, "false", 5) == 0)
    {
        r->p += 5;
        __emit(r, hits, hit_count, AGENT_JSON_VALUE_BOOL, RT_NULL, 0, RT_TRUE, 0);
        return RT_EOK;
    }

    if (c == 'n' && r->end - r->p >= 4 && rt_memcmp(r->p, "null", 4) == 0)
    {
        r->p += 4;
        __emit(r, hits, hit_count, AGENT_JSON_VALUE_NULL, RT_NULL, 0, RT_TRUE, 0);
        return RT_EOK;
    }

    if (c == '-' || (c >= '0' && c <= '9'))
    {
        int number = 0;

        if (__parse_number(r, &number) != RT_EOK)
        {
            return -RT_EINVAL;
        }
        __emit(r, hits, hit_count, AGENT_JSON_VALUE_NUMBER, RT_NULL, 0, RT_TRUE, number);
        return RT_EOK;
    }

    return -RT_EINVAL;
}

rt_err_t agent_json_scan(const char *text, int len,
                         const char *const *watches, int watch_count,
                         agent_json_on_match on_match, void *user)
{
    agent_json_reader_t reader;
    rt_err_t err;

    if (!text || len <= 0 || watch_count < 0 || watch_count > AGENT_JSON_MAX_WATCHES)
    {
        return -RT_EINVAL;
    }

    rt_memset(&reader, 0, sizeof(reader));
    reader.p = text;
    reader.end = text + len;
    reader.watches = watches;
    reader.watch_count = watch_count;
    reader.on_match = on_match;
    reader.user = user;

    __skip_ws(&reader);
    if (reader.p >= reader.end)
    {
        return -RT_EINVAL;
    }

    err = __parse_value(&reader);
    if (err != RT_EOK)
    {
        return err;
    }

    for (;;)
    {
        agent_json_frame_t *frame;

        __skip_ws(&reader);
        if (reader.depth == 0)
        {
            break;
        }

        frame = &reader.stack[reader.depth - 1];
        if (reader.p >= reader.end)
        {
            return -RT_EINVAL;
        }

        if (frame->kind == '{')
        {
            if (frame->state == 0)
            {
                if (*reader.p == '}')
                {
                    /* 逗号后的 '}' 是尾逗号，cJSON 同样拒收 */
                    if (frame->has_item)
                    {
                        return -RT_EINVAL;
                    }
                    reader.p++;
                    reader.depth--;
                    continue;
                }
                err = __parse_key(&reader, frame);
                if (err != RT_EOK)
                {
                    return err;
                }
                frame->has_item = RT_TRUE;
                frame->state = 1;
            }
            else if (frame->state == 1)
            {
                if (*reader.p != ':')
                {
                    return -RT_EINVAL;
                }
                reader.p++;
                frame->state = 2;
            }
            else if (frame->state == 2)
            {
                err = __parse_value(&reader);
                if (err != RT_EOK)
                {
                    return err;
                }
                frame->state = 3;
            }
            else
            {
                if (*reader.p == ',')
                {
                    reader.p++;
                    frame->state = 0;
                }
                else if (*reader.p == '}')
                {
                    reader.p++;
                    reader.depth--;
                }
                else
                {
                    return -RT_EINVAL;
                }
            }
        }
        else
        {
            if (frame->state == 0)
            {
                if (*reader.p == ']')
                {
                    if (frame->has_item)
                    {
                        return -RT_EINVAL;
                    }
                    reader.p++;
                    reader.depth--;
                    continue;
                }
                err = __parse_value(&reader);
                if (err != RT_EOK)
                {
                    return err;
                }
                frame->has_item = RT_TRUE;
                frame->state = 1;
            }
            else
            {
                if (*reader.p == ',')
                {
                    reader.p++;
                    frame->index++;
                    frame->state = 0;
                }
                else if (*reader.p == ']')
                {
                    reader.p++;
                    reader.depth--;
                }
                else
                {
                    return -RT_EINVAL;
                }
            }
        }
    }

    __skip_ws(&reader);
    if (reader.p != reader.end)
    {
        return -RT_EINVAL;
    }

    return RT_EOK;
}
