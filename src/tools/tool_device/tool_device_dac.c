/*
 * tool_device_dac.c — DAC 输出工具
 *
 * 2 个工具封装 rt_dac_write 与 rt_dac_enable/disable。写入值是原始码，位宽由驱动决定，
 * 工具会顺带取 resolution 并把量程写进结果，避免把原始码当成伏特。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_DAC)

#include <rtdevice.h>
#include <drivers/dac.h>

static rt_dac_device_t dac_arg(cJSON *args, AgentToolNode_t node, const char **name_out,
                               rt_uint32_t *channel)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_device_t dev;
    cJSON *item;

    dev = agent_dev_find_class(args, node, name_out, RT_Device_Class_DAC, "dac", RT_FALSE,
                               out, &used);
    if (!dev) return RT_NULL;
    item = cJSON_GetObjectItemCaseSensitive(args, "channel");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'channel' must be a channel number (0, 1, ...)");
        return RT_NULL;
    }
    *channel = (rt_uint32_t)item->valuedouble;
    return (rt_dac_device_t)dev;
}

void tool_dac_write(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_dac_device_t dac;
    rt_device_t dev;
    cJSON *item;
    rt_uint32_t channel = 0;
    rt_uint32_t value;
    rt_uint8_t bits = 0;
    rt_uint32_t full_scale = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dac = dac_arg(args, node, &name, &channel);
    if (!dac) return;
    item = cJSON_GetObjectItemCaseSensitive(args, "value");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'value' must be a non-negative raw code");
        return;
    }
    value = (rt_uint32_t)item->valuedouble;
    dev = (rt_device_t)dac;
    rt_device_control(dev, RT_DAC_CMD_GET_RESOLUTION, &bits);
    if (bits && bits < 32) full_scale = (1u << bits) - 1;
    if (full_scale && value > full_scale)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: value %u is above this %d-bit DAC's full scale %u",
                   (unsigned)value, (int)bits, (unsigned)full_scale);
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }

    result = rt_dac_write(dac, channel, value);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: dac write failed: device=%s channel=%u err=%d",
                   name, (unsigned)channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "dac write ok: device=%s channel=%u raw=%u",
               name, (unsigned)channel, (unsigned)value);
    if (bits) agent_dev_append(out, sizeof(out), &used, " resolution=%dbit", (int)bits);
    agent_dev_append(out, sizeof(out), &used,
        "\nnote: the output holds this code until it is rewritten or the channel is disabled; "
        "this tool does not prove the analog level reached the load");
    agent_dev_finish(node, RT_EOK, out);
}

void tool_dac_enable(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_dac_device_t dac;
    rt_uint32_t channel = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dac = dac_arg(args, node, &name, &channel);
    if (!dac) return;

    result = rt_dac_enable(dac, channel);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: dac enable failed: device=%s channel=%u err=%d",
                   name, (unsigned)channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "dac enabled: device=%s channel=%u",
               name, (unsigned)channel);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_dac_disable(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_dac_device_t dac;
    rt_uint32_t channel = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dac = dac_arg(args, node, &name, &channel);
    if (!dac) return;

    result = rt_dac_disable(dac, channel);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: dac disable failed: device=%s channel=%u err=%d",
                   name, (unsigned)channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "dac disabled: device=%s channel=%u",
               name, (unsigned)channel);
    agent_dev_finish(node, RT_EOK, out);
}

static void dac_write_desc(cJSON *func)
{
    tool_desc(func, "Output a raw code on a DAC channel.");
    tool_desc_more(func, "State-changing: the analog pin moves, so call only after the user "
        "explicitly authorized this peripheral.");
    tool_desc_more(func, "The code's width and full scale come from the driver; the result "
        "reports both.");
}

static void dac_write_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact DAC device name from device_list.");
    create_property(props, "channel", "number", "DAC channel number.");
    create_property(props, "value", "number",
        "Raw output code, 0 .. the driver's full scale (e.g. 0..4095 for a "
        "12-bit DAC).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
    cJSON_AddItemToArray(required, cJSON_CreateString("value"));
}

static void dac_enable_desc(cJSON *func)
{
    tool_desc(func, "Enable a DAC channel.");
    tool_desc_state_changing(func);
}

static void dac_enable_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact DAC device name from device_list.");
    create_property(props, "channel", "number", "DAC channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static void dac_disable_desc(cJSON *func)
{
    tool_desc(func, "Disable a DAC channel.");
    tool_desc_more(func, "State-changing, but the safe direction: the output goes idle.");
}

static void dac_disable_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact DAC device name from device_list.");
    create_property(props, "channel", "number", "DAC channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static const struct agent_tool_def dac_tools[] =
{
    {
        "dac_write",
        dac_write_desc,
        dac_write_params,
        tool_dac_write
    },
    {
        "dac_enable",
        dac_enable_desc,
        dac_enable_params,
        tool_dac_enable
    },
    {
        "dac_disable",
        dac_disable_desc,
        dac_disable_params,
        tool_dac_disable
    }
};

AGENT_TOOL_MODULE_EXPORT(dac, dac_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_DAC */
