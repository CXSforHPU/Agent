#include "utils/utils_internal.h"

/*
 * JSON 直写实现。转义规则与 cJSON 的 print_string_ptr 保持一致，
 * 这样换掉中间树之后报文内容不变。
 */

static void __put(agent_json_writer_t *writer, const char *text, rt_size_t length)
{
    if (!writer || writer->overflow) return;

    if (!writer->buf)
    {
        writer->len += length;
        return;
    }
    if (writer->len + length >= writer->cap)
    {
        writer->overflow = RT_TRUE;
        return;
    }
    rt_memcpy(writer->buf + writer->len, text, length);
    writer->len += length;
}

void agent_json_writer_init(agent_json_writer_t *writer, char *buf, rt_size_t cap)
{
    if (!writer) return;
    writer->buf = buf;
    writer->cap = buf ? cap : 0;
    writer->len = 0;
    writer->overflow = RT_FALSE;
}

rt_bool_t agent_json_writer_ok(const agent_json_writer_t *writer)
{
    return writer && !writer->overflow;
}

rt_size_t agent_json_writer_len(const agent_json_writer_t *writer)
{
    return writer ? writer->len : 0;
}

void agent_json_write_raw(agent_json_writer_t *writer, const char *text, rt_size_t length)
{
    if (!text || !length) return;
    __put(writer, text, length);
}

void agent_json_write_literal(agent_json_writer_t *writer, const char *literal)
{
    if (!literal) return;
    __put(writer, literal, rt_strlen(literal));
}

void agent_json_write_string(agent_json_writer_t *writer, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    rt_size_t run = 0;

    __put(writer, "\"", 1);
    while (*p)
    {
        const char *escape = RT_NULL;
        char unicode[7];

        switch (*p)
        {
        case '\"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (*p < 0x20)
            {
                unicode[0] = '\\';
                unicode[1] = 'u';
                unicode[2] = '0';
                unicode[3] = '0';
                unicode[4] = hex[*p >> 4];
                unicode[5] = hex[*p & 0x0f];
                unicode[6] = '\0';
                escape = unicode;
            }
            break;
        }

        if (escape)
        {
            if (run > 0)
            {
                __put(writer, (const char *)p - run, run);
                run = 0;
            }
            __put(writer, escape, rt_strlen(escape));
        }
        else
        {
            run++;
        }
        p++;
    }
    if (run > 0) __put(writer, (const char *)p - run, run);
    __put(writer, "\"", 1);
}

void agent_json_write_int(agent_json_writer_t *writer, int value)
{
    char number[16];

    rt_snprintf(number, sizeof(number), "%d", value);
    __put(writer, number, rt_strlen(number));
}

void agent_json_write_bool(agent_json_writer_t *writer, rt_bool_t value)
{
    if (value) __put(writer, "true", 4);
    else __put(writer, "false", 5);
}

static void __write_number(agent_json_writer_t *writer, const cJSON *item)
{
    double value = item->valuedouble;

    /* 整数值按整数写，其余交给 cJSON 格式化，避免依赖 rt_snprintf 的 %g */
    if (value == (double)item->valueint)
    {
        agent_json_write_int(writer, item->valueint);
    }
    else
    {
        char *text = cJSON_PrintUnformatted(item);

        if (text)
        {
            __put(writer, text, rt_strlen(text));
            cJSON_free(text);
        }
    }
}

void agent_json_write_cjson(agent_json_writer_t *writer, const cJSON *item)
{
    if (!writer || writer->overflow) return;

    if (!item)
    {
        __put(writer, "null", 4);
        return;
    }

    switch (item->type & 0xff)
    {
    case cJSON_False:
        __put(writer, "false", 5);
        break;
    case cJSON_True:
        __put(writer, "true", 4);
        break;
    case cJSON_NULL:
        __put(writer, "null", 4);
        break;
    case cJSON_Number:
        __write_number(writer, item);
        break;
    case cJSON_String:
        agent_json_write_string(writer, item->valuestring);
        break;
    case cJSON_Array:
    {
        rt_bool_t first = RT_TRUE;
        const cJSON *child;

        __put(writer, "[", 1);
        for (child = item->child; child; child = child->next)
        {
            if (!first) __put(writer, ",", 1);
            first = RT_FALSE;
            agent_json_write_cjson(writer, child);
            if (writer->overflow) return;
        }
        __put(writer, "]", 1);
        break;
    }
    case cJSON_Object:
    {
        rt_bool_t first = RT_TRUE;
        const cJSON *child;

        __put(writer, "{", 1);
        for (child = item->child; child; child = child->next)
        {
            if (!first) __put(writer, ",", 1);
            first = RT_FALSE;
            agent_json_write_string(writer, child->string);
            __put(writer, ":", 1);
            agent_json_write_cjson(writer, child);
            if (writer->overflow) return;
        }
        __put(writer, "}", 1);
        break;
    }
    default:
        __put(writer, "null", 4);
        break;
    }
}
