/*
 * tool_device_core.c — 通用设备框架工具
 *
 * 7 个工具直接封装 rt_device_*，不分设备类别，任何开了 RT_USING_DEVICE 的 BSP 都能用。
 * 参数约定、单位语义、控制台护栏见 docs/tool_device.md 与 skills/device-commands.md。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_CORE)

#include <time.h>
#include <rtdevice.h>
#include <drivers/dev_rtc.h>
#include <drivers/dev_watchdog.h>

typedef enum
{
    CMD_NOARG,     /* driver takes no argument */
    CMD_REFUSED,   /* needs a driver-specific pointer the tool cannot build */
    CMD_GEOM,      /* BLK_GETGEOME: struct rt_device_blk_geometry out */
    CMD_RTC_GET,   /* RTC_GET_TIME: time_t out */
    CMD_RTC_SET,   /* RTC_SET_TIME: time_t in */
    CMD_U32_GET,   /* WDT_GET_TIMEOUT / WDT_GET_TIMELEFT: rt_uint32_t out */
    CMD_U32_SET    /* WDT_SET_TIMEOUT: rt_uint32_t in */
} cmd_kind_t;

struct ctrl_alias
{
    const char *name;
    int value;
    cmd_kind_t kind;
    const char *note;
};

/* Command values follow rtdef.h base commands and
   RT_DEVICE_CTRL_BASE(Type) = (class_id + 1) * 0x100 per class headers. */
static const struct ctrl_alias ctrl_aliases[] =
{
    {"RESUME",              0x01,   CMD_REFUSED, "takes a driver-specific argument"},
    {"SUSPEND",             0x02,   CMD_REFUSED, "takes a driver-specific argument"},
    {"CONFIG",              0x03,   CMD_REFUSED, "takes a driver-specific structure"},
    {"CLOSE",               0x04,   CMD_REFUSED, "takes a driver-specific structure"},
    {"NOTIFY_SET",          0x05,   CMD_REFUSED, "takes a callback structure"},
    {"SET_INT",             0x06,   CMD_REFUSED, "takes a driver-specific interrupt argument"},
    {"CLR_INT",             0x07,   CMD_REFUSED, "takes a driver-specific interrupt argument"},
    {"GET_INT",             0x08,   CMD_REFUSED, "writes through a driver-specific pointer"},
    {"CONSOLE_OFLAG",       0x09,   CMD_REFUSED, "writes through a driver-specific pointer"},
    {"CURSOR_SET_POSITION", 0x10,   CMD_REFUSED, "takes a graphic cursor position structure"},
    {"CURSOR_SET_TYPE",     0x11,   CMD_REFUSED, "takes a graphic cursor type argument"},
    {"CHAR_STREAM",         0x101,  CMD_REFUSED, "argument layout is driver-specific"},
    {"BLK_GETGEOME",        0x201,  CMD_GEOM,    ""},
    {"BLK_SYNC",            0x202,  CMD_NOARG,   ""},
    {"BLK_ERASE",           0x203,  CMD_REFUSED, "takes a sector-range structure"},
    {"BLK_AUTOREFRESH",     0x204,  CMD_REFUSED, "takes a mode argument"},
    {"BLK_PARTITION",       0x205,  CMD_REFUSED, "writes through an output structure"},
    {"NETIF_GETMAC",        0x301,  CMD_REFUSED, "writes through a MAC buffer pointer"},
    {"MTD_FORMAT",          0x401,  CMD_REFUSED, "takes a format argument structure"},
    {"RTC_GET_TIME",        0x601,  CMD_RTC_GET, ""},
    {"RTC_SET_TIME",        0x602,  CMD_RTC_SET, ""},
    {"RTC_GET_TIMEVAL",     0x603,  CMD_REFUSED, "writes through a struct timeval pointer"},
    {"RTC_SET_TIMEVAL",     0x604,  CMD_REFUSED, "takes a struct timeval pointer"},
    {"RTC_GET_ALARM",       0x605,  CMD_REFUSED, "writes through an alarm structure pointer"},
    {"RTC_SET_ALARM",       0x606,  CMD_REFUSED, "takes an alarm structure pointer"},
    {"RTC_GET_TIMESPEC",    0x607,  CMD_REFUSED, "writes through a timespec pointer"},
    {"RTC_SET_TIMESPEC",    0x608,  CMD_REFUSED, "takes a timespec pointer"},
    {"RTC_GET_TIMERES",     0x609,  CMD_REFUSED, "writes through a resolution pointer"},
    {"WDT_GET_TIMEOUT",     0x1D01, CMD_U32_GET, ""},
    {"WDT_SET_TIMEOUT",     0x1D02, CMD_U32_SET, ""},
    {"WDT_GET_TIMELEFT",    0x1D03, CMD_U32_GET, ""},
    {"WDT_KEEPALIVE",       0x1D04, CMD_NOARG,   ""},
    {"WDT_START",           0x1D05, CMD_NOARG,   ""},
    {"WDT_STOP",            0x1D06, CMD_NOARG,   ""},
};

struct list_arg
{
    char *out;
    rt_size_t used;
    int wanted_type;
    int printed;
    int stopped;
};

static void list_cb(rt_device_t dev, void *arg)
{
    struct list_arg *ctx = (struct list_arg *)arg;

    if (ctx->stopped) return;
    if (ctx->wanted_type >= 0 && (int)dev->type != ctx->wanted_type) return;
    if (ctx->printed >= 40 || ctx->used + 64 >= DEVICE_OUT_SIZE)
    {
        agent_dev_append(ctx->out, DEVICE_OUT_SIZE, &ctx->used, "\n... more devices omitted");
        ctx->stopped = 1;
        return;
    }
    agent_dev_append(ctx->out, DEVICE_OUT_SIZE, &ctx->used,
               "\nname=%s class=%s ref=%d %s f=0x%03x/0x%03x",
               dev->parent.name, agent_dev_class_name(dev->type), (int)dev->ref_count,
               (dev->open_flag & RT_DEVICE_OFLAG_OPEN) ? "open" : "closed",
               (unsigned)dev->flag, (unsigned)dev->open_flag);
    ctx->printed++;
}

struct count_arg
{
    int wanted_type;
    int total;
    int matched;
};

static void count_cb(rt_device_t dev, void *arg)
{
    struct count_arg *ctx = (struct count_arg *)arg;

    ctx->total++;
    if (ctx->wanted_type >= 0 && (int)dev->type == ctx->wanted_type) ctx->matched++;
}

void tool_device_list(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    cJSON *filter = cJSON_GetObjectItemCaseSensitive(args_obj, "class");
    struct list_arg ctx;
    struct count_arg counts;
    long wanted = -1;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    if (filter)
    {
        if (cJSON_IsNumber(filter))
        {
            wanted = (long)filter->valuedouble;
        }
        else if (cJSON_IsString(filter))
        {
            if (!agent_dev_parse_num(filter->valuestring, &wanted))
                wanted = agent_dev_class_by_name(filter->valuestring);
            if (wanted < 0)
            {
                agent_dev_finish(node, -RT_EINVAL,
                       "error: unknown class; use 'char','block','rtc','wdt','timer','adc',"
                       "'pwm','sensor',... or a class number");
                return;
            }
        }
        else
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'class' must be a class name or number");
            return;
        }
        if (wanted < 0 || wanted > RT_Device_Class_Unknown)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: class number out of range");
            return;
        }
    }

    counts.wanted_type = (int)wanted;
    counts.total = counts.matched = 0;
    agent_dev_foreach(count_cb, &counts);
    agent_dev_append(out, sizeof(out), &used, "%d device(s)%s:",
               wanted >= 0 ? counts.matched : counts.total,
               wanted >= 0 ? " matching filter" : "");
    ctx.out = out;
    ctx.used = used;
    ctx.wanted_type = (int)wanted;
    ctx.printed = 0;
    ctx.stopped = 0;
    agent_dev_foreach(list_cb, &ctx);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_info(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    rt_device_t console = rt_console_get_device();
    char flags[128];
    char open_flags[128];

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find(args_obj, node, &name, RT_FALSE, out, &used);
    if (!dev) return;

    agent_dev_decode_flags(dev->flag, flags, sizeof(flags));
    agent_dev_decode_flags(dev->open_flag, open_flags, sizeof(open_flags));
    agent_dev_append(out, sizeof(out), &used,
               "device=%s class=%s(%d) console=%s ref_count=%d",
               name, agent_dev_class_name(dev->type), (int)dev->type,
               (console == dev) ? "yes" : "no", (int)dev->ref_count);
    agent_dev_append(out, sizeof(out), &used,
               "\nflag=0x%03x [%s] open_flag=0x%03x [%s] open=%s",
               (unsigned)dev->flag, flags, (unsigned)dev->open_flag, open_flags,
               (dev->open_flag & RT_DEVICE_OFLAG_OPEN) ? "yes" : "no");
#ifdef RT_USING_DEVICE_OPS
    if (dev->ops)
    {
        agent_dev_append(out, sizeof(out), &used, "\nops=");
        if (dev->ops->init)    agent_dev_append(out, sizeof(out), &used, "init,");
        if (dev->ops->open)    agent_dev_append(out, sizeof(out), &used, "open,");
        if (dev->ops->close)   agent_dev_append(out, sizeof(out), &used, "close,");
        if (dev->ops->read)    agent_dev_append(out, sizeof(out), &used, "read,");
        if (dev->ops->write)   agent_dev_append(out, sizeof(out), &used, "write,");
        if (dev->ops->control) agent_dev_append(out, sizeof(out), &used, "control,");
        if (used > 0 && out[used - 1] == ',') out[--used] = '\0';
    }
    else
    {
        agent_dev_append(out, sizeof(out), &used, "\nops=none (legacy call hooks)");
    }
#endif
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_open(cJSON *args_obj, AgentToolNode_t node)
{
    static const struct { const char *name; rt_uint16_t bit; } flag_bits[] =
    {
        {"stream", RT_DEVICE_FLAG_STREAM}, {"int_rx", RT_DEVICE_FLAG_INT_RX},
        {"dma_rx", RT_DEVICE_FLAG_DMA_RX}, {"int_tx", RT_DEVICE_FLAG_INT_TX},
        {"dma_tx", RT_DEVICE_FLAG_DMA_TX},
    };
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *name_node = cJSON_GetObjectItemCaseSensitive(args_obj, "name");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(args_obj, "mode");
    cJSON *flags = cJSON_GetObjectItemCaseSensitive(args_obj, "flags");
    rt_uint16_t oflag = RT_DEVICE_FLAG_RDWR;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    if (!cJSON_IsString(name_node) || name_node->valuestring[0] == '\0')
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'name' must be a non-empty device name string");
        return;
    }
    name = name_node->valuestring;
    if (mode)
    {
        if (!cJSON_IsString(mode))
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'mode' must be 'rdonly', 'wronly' or 'rdwr'");
            return;
        }
        if (rt_strcmp(mode->valuestring, "rdonly") == 0) oflag = RT_DEVICE_FLAG_RDONLY;
        else if (rt_strcmp(mode->valuestring, "wronly") == 0) oflag = RT_DEVICE_FLAG_WRONLY;
        else if (rt_strcmp(mode->valuestring, "rdwr") == 0) oflag = RT_DEVICE_FLAG_RDWR;
        else
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'mode' must be 'rdonly', 'wronly' or 'rdwr'");
            return;
        }
    }
    if (flags)
    {
        int i, n;

        if (!cJSON_IsArray(flags))
        {
            agent_dev_finish(node, -RT_EINVAL,
                   "error: 'flags' must be an array of 'stream','int_rx','dma_rx','int_tx','dma_tx'");
            return;
        }
        n = cJSON_GetArraySize(flags);
        for (i = 0; i < n; i++)
        {
            cJSON *item = cJSON_GetArrayItem(flags, i);
            size_t j;
            int matched = 0;

            if (!cJSON_IsString(item))
            {
                agent_dev_finish(node, -RT_EINVAL, "error: every 'flags' entry must be a string");
                return;
            }
            for (j = 0; j < sizeof(flag_bits) / sizeof(flag_bits[0]); j++)
            {
                if (rt_strcmp(item->valuestring, flag_bits[j].name) == 0)
                {
                    oflag |= flag_bits[j].bit;
                    matched = 1;
                    break;
                }
            }
            if (!matched)
            {
                agent_dev_finish(node, -RT_EINVAL,
                       "error: unknown open flag; use 'stream','int_rx','dma_rx','int_tx','dma_tx'");
                return;
            }
        }
    }

    dev = rt_device_find(name);
    if (!dev)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' not found; call device_list for exact names", name);
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }

    result = rt_device_open(dev, oflag);
    if (result != RT_EOK && result != -RT_ENOSYS)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: open failed: device=%s err=%d", name, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "open ok: device=%s oflag=0x%03x ref_count=%d open_flag=0x%03x",
               name, (unsigned)oflag, (int)dev->ref_count, (unsigned)dev->open_flag);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_close(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    rt_device_t console = rt_console_get_device();
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find(args_obj, node, &name, RT_FALSE, out, &used);
    if (!dev) return;
    if (console == dev)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is the console; this tool refuses to close it "
                   "because it carries this shell", name);
        agent_dev_finish(node, -RT_EPERM, out);
        return;
    }
    if (dev->ref_count == 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: device '%s' is not opened", name);
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }

    result = rt_device_close(dev);
    if (result != RT_EOK && result != -RT_ENOSYS)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: close failed: device=%s err=%d", name, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "close ok: device=%s ref_count=%d", name, (int)dev->ref_count);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_read(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    rt_device_t console = rt_console_get_device();
    cJSON *count_node = cJSON_GetObjectItemCaseSensitive(args_obj, "count");
    cJSON *pos_node = cJSON_GetObjectItemCaseSensitive(args_obj, "pos");
    long count = -1, pos = 0;
    rt_size_t bytes = 0, items = 0, bps = 0, clamp_limit = 0;
    rt_bool_t is_block, clamped = RT_FALSE;
    rt_uint8_t *buffer = RT_NULL;
    rt_ssize_t got;
    rt_size_t preview;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find(args_obj, node, &name, RT_FALSE, out, &used);
    if (!dev) return;
    if (console == dev)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is the console; reading it would consume shell input", name);
        agent_dev_finish(node, -RT_EPERM, out);
        return;
    }
    if (dev->ref_count == 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is not opened; call device_open first", name);
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }
    if ((dev->open_flag & RT_DEVICE_OFLAG_WRONLY) && !(dev->open_flag & RT_DEVICE_OFLAG_RDONLY))
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is opened write-only", name);
        agent_dev_finish(node, -RT_EPERM, out);
        return;
    }
    if (count_node)
    {
        if (!cJSON_IsNumber(count_node) || count_node->valuedouble < 1)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'count' must be a positive number");
            return;
        }
        count = (long)count_node->valuedouble;
    }
    if (pos_node)
    {
        if (!cJSON_IsNumber(pos_node) || pos_node->valuedouble < 0)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'pos' must be a non-negative number");
            return;
        }
        pos = (long)pos_node->valuedouble;
    }

    is_block = (dev->type == RT_Device_Class_Block) ? RT_TRUE : RT_FALSE;
    if (is_block)
    {
        struct rt_device_blk_geometry geometry;

        rt_memset(&geometry, 0, sizeof(geometry));
        if (rt_device_control(dev, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry) != RT_EOK ||
            geometry.bytes_per_sector == 0)
        {
            agent_dev_append(out, sizeof(out), &used,
                       "error: cannot read: device '%s' did not report block geometry", name);
            agent_dev_finish(node, -RT_ERROR, out);
            return;
        }
        bps = geometry.bytes_per_sector;
        items = (count > 0) ? (rt_size_t)count : 1;
        if (items > DEVICE_IO_MAX / bps)
        {
            items = DEVICE_IO_MAX / bps;
            if (items == 0) items = 1;
            clamped = RT_TRUE;
            clamp_limit = DEVICE_IO_MAX;
        }
        bytes = items * bps;
    }
    else
    {
        items = (count > 0) ? (rt_size_t)count : 64;
        if (items > DEVICE_READ_MAX)
        {
            items = DEVICE_READ_MAX;
            clamped = RT_TRUE;
            clamp_limit = DEVICE_READ_MAX;
        }
        bytes = items;
    }

    buffer = (rt_uint8_t *)rt_malloc(bytes ? bytes : 1);
    if (!buffer)
    {
        agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
        return;
    }
    got = rt_device_read(dev, pos, buffer, items);
    if (got < 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: read failed: device=%s err=%d", name, (int)got);
        rt_free(buffer);
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }

    agent_dev_append(out, sizeof(out), &used,
               "read ok: device=%s unit=%s count=%d%s",
               name, is_block ? "blocks" : "bytes", (int)got,
               (got == 0) ? " (no data available)" : "");
    if (is_block)
        agent_dev_append(out, sizeof(out), &used, " (%d bytes)", (int)((rt_size_t)got * bps));
    if (clamped)
        agent_dev_append(out, sizeof(out), &used,
                   "\nnote: request clamped to the %d-byte transfer limit", (int)clamp_limit);

    if (got > 0)
    {
        preview = is_block ? (rt_size_t)got * bps : (rt_size_t)got;
        agent_dev_hex_preview(out, sizeof(out), &used, buffer, preview);
    }
    rt_free(buffer);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_write(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *data_node = cJSON_GetObjectItemCaseSensitive(args_obj, "data");
    cJSON *encoding = cJSON_GetObjectItemCaseSensitive(args_obj, "encoding");
    cJSON *pos_node = cJSON_GetObjectItemCaseSensitive(args_obj, "pos");
    const char *data;
    rt_uint8_t *buffer = RT_NULL;
    rt_size_t bytes = 0, items = 0, bps = 0;
    long pos = 0;
    rt_bool_t is_block;
    rt_ssize_t written;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find(args_obj, node, &name, RT_TRUE, out, &used);
    if (!dev) return;
    if ((dev->open_flag & RT_DEVICE_OFLAG_RDONLY) && !(dev->open_flag & RT_DEVICE_OFLAG_WRONLY))
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is opened read-only", name);
        agent_dev_finish(node, -RT_EPERM, out);
        return;
    }
    if (!cJSON_IsString(data_node) || data_node->valuestring[0] == '\0')
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'data' must be a non-empty string (text, or hex with encoding='hex')");
        return;
    }
    data = data_node->valuestring;
    if (encoding)
    {
        if (!cJSON_IsString(encoding) ||
            (rt_strcmp(encoding->valuestring, "text") != 0 &&
             rt_strcmp(encoding->valuestring, "hex") != 0))
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'encoding' must be 'text' or 'hex'");
            return;
        }
    }
    if (pos_node)
    {
        if (!cJSON_IsNumber(pos_node) || pos_node->valuedouble < 0)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'pos' must be a non-negative number");
            return;
        }
        pos = (long)pos_node->valuedouble;
    }

    if (encoding && rt_strcmp(encoding->valuestring, "hex") == 0)
    {
        int decoded;

        buffer = (rt_uint8_t *)rt_malloc(DEVICE_IO_MAX);
        if (!buffer)
        {
            agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
            return;
        }
        decoded = agent_dev_hex_decode(data, buffer, DEVICE_IO_MAX, out, sizeof(out));
        if (decoded < 0)
        {
            rt_free(buffer);
            agent_dev_finish(node, -RT_EINVAL, out);
            return;
        }
        bytes = (rt_size_t)decoded;
    }
    else
    {
        bytes = rt_strlen(data);
        if (bytes > DEVICE_IO_MAX)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: data too large (limit 2048 bytes)");
            return;
        }
        buffer = (rt_uint8_t *)rt_malloc(bytes);
        if (!buffer)
        {
            agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
            return;
        }
        rt_memcpy(buffer, data, bytes);
    }

    if (bytes == 0)
    {
        rt_free(buffer);
        agent_dev_finish(node, -RT_EINVAL, "error: nothing to write after decoding");
        return;
    }

    is_block = (dev->type == RT_Device_Class_Block) ? RT_TRUE : RT_FALSE;
    if (is_block)
    {
        struct rt_device_blk_geometry geometry;

        rt_memset(&geometry, 0, sizeof(geometry));
        if (rt_device_control(dev, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry) != RT_EOK ||
            geometry.bytes_per_sector == 0)
        {
            agent_dev_append(out, sizeof(out), &used,
                       "error: cannot write: device '%s' did not report block geometry", name);
            rt_free(buffer);
            agent_dev_finish(node, -RT_ERROR, out);
            return;
        }
        bps = geometry.bytes_per_sector;
        if (bytes % bps != 0)
        {
            agent_dev_append(out, sizeof(out), &used,
                       "error: block write needs whole blocks; %d bytes is not a multiple of "
                       "the %d-byte block size", (int)bytes, (int)bps);
            rt_free(buffer);
            agent_dev_finish(node, -RT_EINVAL, out);
            return;
        }
        items = bytes / bps;
    }
    else
    {
        items = bytes;
    }

    written = rt_device_write(dev, pos, buffer, items);
    rt_free(buffer);
    if (written < 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: write failed: device=%s err=%d", name, (int)written);
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }
    if (is_block)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "write %s: device=%s unit=blocks written=%d/%d (%d bytes)",
                   ((rt_size_t)written == items) ? "ok" : "partial",
                   name, (int)written, (int)items, (int)((rt_size_t)written * bps));
    }
    else
    {
        agent_dev_append(out, sizeof(out), &used,
                   "write %s: device=%s unit=bytes written=%d/%d",
                   ((rt_size_t)written == items) ? "ok" : "partial",
                   name, (int)written, (int)items);
    }
    if ((rt_size_t)written != items)
        agent_dev_append(out, sizeof(out), &used, "\nnote: the device accepted fewer bytes than requested");
    agent_dev_finish(node, RT_EOK, out);
}

void tool_device_control(cJSON *args_obj, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    char cmd_display[32];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *cmd_node = cJSON_GetObjectItemCaseSensitive(args_obj, "cmd");
    cJSON *arg_node = cJSON_GetObjectItemCaseSensitive(args_obj, "arg");
    const struct ctrl_alias *alias = RT_NULL;
    long cmd_value = 0;
    cmd_kind_t kind = CMD_NOARG;
    long arg_value = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);

    if (!cJSON_IsObject(args_obj))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find(args_obj, node, &name, RT_TRUE, out, &used);
    if (!dev) return;

    if (cJSON_IsNumber(cmd_node))
    {
        cmd_value = (long)cmd_node->valuedouble;
        rt_snprintf(cmd_display, sizeof(cmd_display), "0x%x", (int)cmd_value);
    }
    else if (cJSON_IsString(cmd_node))
    {
        size_t i;

        if (agent_dev_parse_num(cmd_node->valuestring, &cmd_value))
        {
            rt_snprintf(cmd_display, sizeof(cmd_display), "0x%x", (int)cmd_value);
        }
        else
        {
            for (i = 0; i < sizeof(ctrl_aliases) / sizeof(ctrl_aliases[0]); i++)
            {
                if (rt_strcmp(cmd_node->valuestring, ctrl_aliases[i].name) == 0)
                {
                    alias = &ctrl_aliases[i];
                    break;
                }
            }
            if (!alias)
            {
                agent_dev_append(out, sizeof(out), &used,
                           "error: unknown command '%s'; load skill 'device-commands' for names, "
                           "or pass a number", cmd_node->valuestring);
                agent_dev_finish(node, -RT_EINVAL, out);
                return;
            }
            rt_snprintf(cmd_display, sizeof(cmd_display), "%s(0x%x)",
                        alias->name, alias->value);
            cmd_value = alias->value;
            kind = alias->kind;
        }
    }
    else
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'cmd' must be a command name from skill 'device-commands' or a number");
        return;
    }

    if (alias && alias->kind == CMD_REFUSED)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: cmd '%s' %s; this tool cannot build its argument, not supported "
                   "(see skill 'device-commands')", alias->name, alias->note);
        agent_dev_finish(node, -RT_ENOSYS, out);
        return;
    }

    if (arg_node)
    {
        if (!cJSON_IsNumber(arg_node))
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'arg' must be a number");
            return;
        }
        arg_value = (long)arg_node->valuedouble;
        if (kind != CMD_RTC_SET && kind != CMD_U32_SET)
        {
            agent_dev_append(out, sizeof(out), &used,
                       "error: cmd '%s' takes no argument", cmd_display);
            agent_dev_finish(node, -RT_EINVAL, out);
            return;
        }
    }
    else if (kind == CMD_RTC_SET || kind == CMD_U32_SET)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: cmd '%s' requires a numeric 'arg'", cmd_display);
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }

    switch (kind)
    {
    case CMD_GEOM:
    {
        struct rt_device_blk_geometry geometry;

        rt_memset(&geometry, 0, sizeof(geometry));
        result = rt_device_control(dev, (int)cmd_value, &geometry);
        if (result != RT_EOK) break;
        agent_dev_append(out, sizeof(out), &used,
                   "control ok: device=%s cmd=%s\n"
                   "geometry: sector_count=%u bytes_per_sector=%d block_size=%d",
                   name, cmd_display, (unsigned)geometry.sector_count,
                   (int)geometry.bytes_per_sector, (int)geometry.block_size);
        agent_dev_finish(node, RT_EOK, out);
        return;
    }
    case CMD_RTC_GET:
    {
        time_t seconds = 0;

        result = rt_device_control(dev, (int)cmd_value, &seconds);
        if (result != RT_EOK) break;
        agent_dev_append(out, sizeof(out), &used,
                   "control ok: device=%s cmd=%s\ntime: seconds=%ld",
                   name, cmd_display, (long)seconds);
        agent_dev_finish(node, RT_EOK, out);
        return;
    }
    case CMD_RTC_SET:
    {
        time_t seconds = (time_t)arg_value;

        result = rt_device_control(dev, (int)cmd_value, &seconds);
        if (result != RT_EOK) break;
        agent_dev_append(out, sizeof(out), &used,
                   "control ok: device=%s cmd=%s seconds=%ld",
                   name, cmd_display, (long)seconds);
        agent_dev_finish(node, RT_EOK, out);
        return;
    }
    case CMD_U32_GET:
    {
        rt_uint32_t value = 0;

        result = rt_device_control(dev, (int)cmd_value, &value);
        if (result != RT_EOK) break;
        agent_dev_append(out, sizeof(out), &used,
                   "control ok: device=%s cmd=%s\nvalue: %u",
                   name, cmd_display, (unsigned)value);
        agent_dev_finish(node, RT_EOK, out);
        return;
    }
    case CMD_U32_SET:
    {
        rt_uint32_t value = (rt_uint32_t)arg_value;

        result = rt_device_control(dev, (int)cmd_value, &value);
        if (result != RT_EOK) break;
        agent_dev_append(out, sizeof(out), &used,
                   "control ok: device=%s cmd=%s value=%u",
                   name, cmd_display, (unsigned)value);
        agent_dev_finish(node, RT_EOK, out);
        return;
    }
    default:
        result = rt_device_control(dev, (int)cmd_value, RT_NULL);
        break;
    }

    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "control failed: device=%s cmd=%s err=%d",
                   name, cmd_display, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "control ok: device=%s cmd=%s (no result payload)",
               name, cmd_display);
    agent_dev_finish(node, RT_EOK, out);
}

/* 每个工具的参数对象写成静态 JSON，注册时才解析成 cJSON，省掉十几个临时对象。 */
static void device_list_desc(cJSON *func)
{
    tool_desc(func, "List devices registered in the RT-Thread device framework with name, class, "
        "flags, open state and reference count.");
    tool_desc_more(func, "Always call this first to learn exact device names before touching any "
        "peripheral.");
    tool_desc_read_only(func);
}

static void device_list_params(cJSON *props, cJSON *required)
{
    create_property(props, "class", "string",
        "Optional class filter: 'char','block','rtc','wdt','timer','adc','pwm','sensor',... or a "
        "class number. Omit to list every device.");
}

static void device_info_desc(cJSON *func)
{
    tool_desc(func, "Show one device: class, capability flags, open state, reference count, which "
        "framework operations (init/open/close/read/write/control) its driver implements, and "
        "whether it is the console device.");
    tool_desc_more(func, "Read-only; use before opening to confirm the device fits the task.");
}

static void device_info_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

static void device_open_desc(cJSON *func)
{
    tool_desc(func, "Open a device for use.");
    tool_desc_more(func, "Reference counted: opening twice requires closing twice, and the first "
        "open initializes the device.");
    tool_desc_more(func, "Close it when done.");
}

static void device_open_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    create_property(props, "mode", "string",
        "'rdonly' (reads only), 'wronly', or 'rdwr' (default). Use 'rdonly' unless a write is "
        "actually needed.");
    create_property(props, "flags", "array",
        "Optional list of open flags: 'stream','int_rx','dma_rx','int_tx','dma_tx'. 'int_rx' makes "
        "char reads return buffered data instead of blocking.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

static void device_close_desc(cJSON *func)
{
    tool_desc(func, "Close a device once (reference counted).");
    tool_desc_more(func, "Refuses to close the console device because that would stop this "
        "shell.");
    tool_desc_more(func, "Call it after finishing work on a peripheral.");
}

static void device_close_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

static void device_read_desc(cJSON *func)
{
    tool_desc(func, "Read from an opened device; returns hex and printable text.");
    tool_desc_more(func, "Units follow the RT-Thread device framework: char devices read bytes "
        "and ignore pos, block devices read whole blocks starting at pos.");
    tool_desc_more(func, "Refuses the console device because reads would consume shell input.");
    tool_desc_more(func, "Reports only what the device actually returned.");
}

static void device_read_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    create_property(props, "count", "number",
        "How much to read: bytes for char devices (default 64, max 512), blocks for block devices "
        "(default 1).");
    create_property(props, "pos", "number",
        "Starting block for block devices (default 0); ignored on char "
        "devices.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

static void device_write_desc(cJSON *func)
{
    tool_desc(func, "Write to an opened device.");
    tool_desc_more(func, "Block writes must be whole blocks.");
    tool_desc_more(func, "Call only after the user explicitly authorized writing to this "
        "peripheral; a successful write reports bytes accepted by the framework, not proof the "
        "peripheral acted on them.");
}

static void device_write_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    create_property(props, "data", "string",
        "Bytes to write: plain text, or hex digits with encoding='hex' "
        "(e.g. 'a1 0b').");
    create_property(props, "encoding", "string", "'text' (default) or 'hex'.");
    create_property(props, "pos", "number",
        "Starting block for block devices (default 0); ignored on char "
        "devices.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("data"));
}

static void device_control_desc(cJSON *func)
{
    tool_desc(func, "Run a framework control command on an opened device.");
    tool_desc_more(func, "Value-returning commands supported: BLK_GETGEOME (block geometry), "
        "RTC_GET_TIME, WDT_GET_TIMEOUT, WDT_GET_TIMELEFT; other supported commands report "
        "success/failure only.");
    tool_desc_more(func, "Load skill 'device-commands' first for the command catalog.");
    tool_desc_more(func, "State-changing commands need explicit user authorization.");
}

static void device_control_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact device name from device_list.");
    create_property(props, "cmd", "string",
        "Command name from skill 'device-commands' (e.g. 'BLK_GETGEOME', 'RTC_GET_TIME', "
        "'WDT_KEEPALIVE') or a raw command number.");
    create_property(props, "arg", "number",
        "Value for commands that take one ('RTC_SET_TIME', 'WDT_SET_TIMEOUT'); all other commands "
        "take no argument.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("cmd"));
}

static const struct agent_tool_def core_tools[] =
{
    {
        "device_list",
        device_list_desc,
        device_list_params,
        tool_device_list
    },
    {
        "device_info",
        device_info_desc,
        device_info_params,
        tool_device_info
    },
    {
        "device_open",
        device_open_desc,
        device_open_params,
        tool_device_open
    },
    {
        "device_close",
        device_close_desc,
        device_close_params,
        tool_device_close
    },
    {
        "device_read",
        device_read_desc,
        device_read_params,
        tool_device_read
    },
    {
        "device_write",
        device_write_desc,
        device_write_params,
        tool_device_write
    },
    {
        "device_control",
        device_control_desc,
        device_control_params,
        tool_device_control
    }
};

AGENT_TOOL_MODULE_EXPORT(core, core_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_CORE */
