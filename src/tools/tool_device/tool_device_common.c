/*
 * tool_device_common.c — 设备工具各模块共用的助手
 *
 * 结果缓冲、数字解析、设备遍历与查找、十六进制编解码、读结果预览。
 * 语义与 rt_device_* 保持一致，规则见 docs/tool_device.md。
 */

#include "tool_device_internal.h"

#include <stdarg.h>
#include <rtdevice.h>

void agent_dev_reset_result(AgentToolNode_t node)
{
    messages_destroy(node->ret.messages);
    node->ret.messages = RT_NULL;
    node->ret.ret = -RT_ERROR;
}

void agent_dev_finish(AgentToolNode_t node, rt_err_t result, const char *text)
{
    node->ret.ret = result;
    node->ret.messages = messages_create(1);
    if (!node->ret.messages)
    {
        node->ret.ret = -RT_ENOMEM;
        return;
    }
    if (messages_append(node->ret.messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
        node->ret.ret = -RT_ENOMEM;
    }
}

void agent_dev_append(char *out, rt_size_t size, rt_size_t *used, const char *fmt, ...)
{
    va_list args;
    int written;

    if (*used >= size) return;
    va_start(args, fmt);
    written = rt_vsnprintf(out + *used, size - *used, fmt, args);
    va_end(args);
    if (written < 0) return;
    if ((rt_size_t)written >= size - *used) *used = size - 1;
    else *used += written;
}

int agent_dev_parse_num(const char *text, long *value)
{
    long result = 0;
    int base = 10;
    int digits = 0;

    if (!text || !value) return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text += 2;
    }
    while (*text)
    {
        int ch = (unsigned char)*text++;
        int n;

        if (ch >= '0' && ch <= '9') n = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') n = ch - 'a' + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F') n = ch - 'A' + 10;
        else return 0;
        result = result * base + n;
        digits++;
    }
    if (!digits) return 0;
    *value = result;
    return 1;
}

static const char *class_names[] =
{
    "char", "block", "netif", "mtd", "can", "rtc", "sound", "graphic", "i2cbus",
    "usbdevice", "usbhost", "usbotg", "spibus", "spidevice", "sdio", "pm", "pipe",
    "portal", "timer", "misc", "sensor", "touch", "phy", "security", "wlan", "pin",
    "adc", "dac", "wdt", "pwm", "bus", "unknown"
};

const char *agent_dev_class_name(int type)
{
    if (type >= 0 && (size_t)type < sizeof(class_names) / sizeof(class_names[0]))
        return class_names[type];
    return "unknown";
}

int agent_dev_class_by_name(const char *name)
{
    size_t i;

    if (!name) return -1;
    for (i = 0; i < sizeof(class_names) / sizeof(class_names[0]); i++)
    {
        if (rt_strcmp(name, class_names[i]) == 0) return (int)i;
    }
    return -1;
}

int agent_dev_class_count(void)
{
    return (int)(sizeof(class_names) / sizeof(class_names[0]));
}

void agent_dev_decode_flags(rt_uint16_t flags, char *out, rt_size_t size)
{
    static const struct { rt_uint16_t bit; const char *name; } bits[] =
    {
        {RT_DEVICE_FLAG_REMOVABLE, "removable"}, {RT_DEVICE_FLAG_STANDALONE, "standalone"},
        {RT_DEVICE_FLAG_ACTIVATED, "activated"}, {RT_DEVICE_FLAG_SUSPENDED, "suspended"},
        {RT_DEVICE_FLAG_STREAM, "stream"},       {RT_DEVICE_FLAG_DYNAMIC, "dynamic"},
        {RT_DEVICE_FLAG_INT_RX, "int_rx"},       {RT_DEVICE_FLAG_DMA_RX, "dma_rx"},
        {RT_DEVICE_FLAG_INT_TX, "int_tx"},       {RT_DEVICE_FLAG_DMA_TX, "dma_tx"},
    };
    rt_size_t used = 0;
    size_t i;

    out[0] = '\0';
    switch (flags & RT_DEVICE_FLAG_RDWR)
    {
    case RT_DEVICE_FLAG_RDWR:   agent_dev_append(out, size, &used, "rdwr"); break;
    case RT_DEVICE_FLAG_RDONLY: agent_dev_append(out, size, &used, "rdonly"); break;
    case RT_DEVICE_FLAG_WRONLY: agent_dev_append(out, size, &used, "wronly"); break;
    default:                    agent_dev_append(out, size, &used, "no-rw"); break;
    }
    for (i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        if (flags & bits[i].bit) agent_dev_append(out, size, &used, ",%s", bits[i].name);
    }
}

int agent_dev_foreach(void (*callback)(rt_device_t dev, void *arg), void *arg)
{
    struct rt_object_information *info;
    struct rt_list_node *node;
    int count = 0;

    info = rt_object_get_information(RT_Object_Class_Device);
    if (!info) return 0;
    rt_enter_critical();
    rt_list_for_each(node, &info->object_list)
    {
        struct rt_object *obj = rt_list_entry(node, struct rt_object, list);

        if ((obj->type & ~RT_Object_Class_Static) != RT_Object_Class_Device) continue;
        callback((rt_device_t)obj, arg);
        count++;
    }
    rt_exit_critical();
    return count;
}

rt_device_t agent_dev_find(cJSON *args, AgentToolNode_t node, const char **name_out,
                           rt_bool_t need_open, char *out, rt_size_t *used)
{
    cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
    rt_device_t dev;

    if (!cJSON_IsObject(args) || !cJSON_IsString(name) || name->valuestring[0] == '\0')
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'name' must be a non-empty device name string");
        return RT_NULL;
    }
    *name_out = name->valuestring;
    dev = rt_device_find(name->valuestring);
    if (!dev)
    {
        agent_dev_append(out, DEVICE_OUT_SIZE, used,
                         "error: device '%s' not found; call device_list for exact names",
                         name->valuestring);
        agent_dev_finish(node, -RT_ERROR, out);
        return RT_NULL;
    }
    if (need_open && dev->ref_count == 0)
    {
        agent_dev_append(out, DEVICE_OUT_SIZE, used,
                         "error: device '%s' is not opened; call device_open first",
                         name->valuestring);
        agent_dev_finish(node, -RT_ERROR, out);
        return RT_NULL;
    }
    return dev;
}

rt_device_t agent_dev_find_class(cJSON *args, AgentToolNode_t node, const char **name_out,
                                 int class_id, const char *class_label, rt_bool_t need_open,
                                 char *out, rt_size_t *used)
{
    rt_device_t dev = agent_dev_find(args, node, name_out, need_open, out, used);

    if (!dev) return RT_NULL;
    if ((int)dev->type != class_id)
    {
        agent_dev_append(out, DEVICE_OUT_SIZE, used,
                         "error: device '%s' is a %s device, this tool needs a %s device; "
                         "call device_list with class=\"%s\" for candidates",
                         *name_out, agent_dev_class_name(dev->type), class_label, class_label);
        agent_dev_finish(node, -RT_EINVAL, out);
        return RT_NULL;
    }
    return dev;
}

int agent_dev_hex_decode(const char *text, rt_uint8_t *buffer, rt_size_t max,
                         char *err, rt_size_t err_size)
{
    const char *p = text;
    rt_size_t bytes = 0;
    int hi = -1;

    err[0] = '\0';
    while (*p)
    {
        int ch = (unsigned char)*p++;
        int v;

        if (ch == ' ' || ch == ':' || ch == ',' || ch == '\t') continue;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else
        {
            rt_snprintf(err, err_size,
                        "error: invalid hex data; use pairs of hex digits, e.g. 'a1 0b ff'");
            return -RT_EINVAL;
        }
        if (hi < 0) hi = v;
        else
        {
            if (bytes >= max)
            {
                rt_snprintf(err, err_size, "error: hex data too large (limit %d bytes)",
                            (int)max);
                return -RT_EINVAL;
            }
            buffer[bytes++] = (rt_uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0)
    {
        rt_snprintf(err, err_size, "error: invalid hex data; odd digit count");
        return -RT_EINVAL;
    }
    return (int)bytes;
}

void agent_dev_hex_preview(char *out, rt_size_t size, rt_size_t *used,
                           const rt_uint8_t *data, rt_size_t len)
{
    rt_size_t preview = (len > DEVICE_PREVIEW_MAX) ? DEVICE_PREVIEW_MAX : len;
    rt_size_t i;

    if (preview == 0) return;
    agent_dev_append(out, size, used, "\nhex:");
    for (i = 0; i < preview; i++)
        agent_dev_append(out, size, used, " %02x", data[i]);
    agent_dev_append(out, size, used, "\ntext: ");
    for (i = 0; i < preview; i++)
    {
        char ch = (char)data[i];

        agent_dev_append(out, size, used, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
    }
    if (len > preview)
        agent_dev_append(out, size, used, "\n(%d more bytes not shown)", (int)(len - preview));
}
