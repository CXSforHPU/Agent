/*
 * tool_device_adc.c — ADC 采样工具
 *
 * 1 个工具封装 rt_adc_enable/read/voltage/disable：使能通道、转换一次、读电压、再关掉通道，
 * 所以调用完不会把通道留在开启状态。原始码的位宽由驱动决定，工具顺带取 resolution 与 vref。
 */
#include "tool_device_internal.h"

/* ADC V2 换了 rt_adc_enable/read/voltage 的签名（改成按通道数组批量采样），本模块只对接
 * V1 框架；开了 RT_USING_ADC_V2 的工程这里整文件不参与编译，Kconfig 也已禁掉该开关。 */
#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_ADC) && !defined(RT_USING_ADC_V2)

#include <rtdevice.h>
#include <drivers/adc.h>

void tool_adc_read(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    rt_adc_device_t adc;
    cJSON *item;
    rt_int8_t channel;
    rt_uint32_t value = 0;
    rt_int16_t mv;
    rt_uint8_t bits = 0;
    rt_int16_t vref = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = agent_dev_find_class(args, node, &name, RT_Device_Class_ADC, "adc", RT_FALSE, out, &used);
    if (!dev) return;
    item = cJSON_GetObjectItemCaseSensitive(args, "channel");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'channel' must be a channel number (0, 1, ...)");
        return;
    }
    channel = (rt_int8_t)item->valuedouble;
    adc = (rt_adc_device_t)dev;

    result = rt_adc_enable(adc, channel);
    if (result != RT_EOK && result != -RT_ENOSYS)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: channel enable failed: device=%s channel=%d err=%d",
                   name, (int)channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    value = rt_adc_read(adc, channel);
    mv = rt_adc_voltage(adc, channel);
    rt_device_control(dev, RT_ADC_CMD_GET_RESOLUTION, &bits);
    rt_device_control(dev, RT_ADC_CMD_GET_VREF, &vref);
    if (result == RT_EOK) rt_adc_disable(adc, channel);

    agent_dev_append(out, sizeof(out), &used, "adc read: device=%s channel=%d raw=%u",
               name, (int)channel, (unsigned)value);
    if (bits) agent_dev_append(out, sizeof(out), &used, " resolution=%dbit", (int)bits);
    if (vref > 0) agent_dev_append(out, sizeof(out), &used, " vref=%dmV", (int)vref);
    if (mv >= 0) agent_dev_append(out, sizeof(out), &used, "\nvoltage=%dmV", (int)mv);
    agent_dev_finish(node, RT_EOK, out);
}

static void adc_read_desc(cJSON *func)
{
    tool_desc(func, "Sample one ADC channel: enables it, converts once, reads the raw code and "
        "the scaled voltage, then disables it again, so no state is left behind.");
    tool_desc_read_only(func);
}

static void adc_read_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact ADC device name from device_list.");
    create_property(props, "channel", "number", "ADC channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static const struct agent_tool_def adc_tools[] =
{
    {
        "adc_read",
        adc_read_desc,
        adc_read_params,
        tool_adc_read
    }
};

AGENT_TOOL_MODULE_EXPORT(adc, adc_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_ADC && !RT_USING_ADC_V2 */
