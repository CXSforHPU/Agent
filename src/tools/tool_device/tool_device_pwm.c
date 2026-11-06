/*
 * tool_device_pwm.c — PWM 通道工具
 *
 * 4 个工具封装 rt_pwm_set/get/enable/disable，按设备名 + 通道号操作。
 * 单位沿用框架：period/pulse 都是纳秒，占空比 = pulse / period。
 * PWM 走的是 control 通路，不要求先 device_open。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_PWM)

#include <rtdevice.h>
#include <drivers/dev_pwm.h>

/* 取出 PWM 设备与通道号；失败时结果已经写好，返回 RT_NULL。 */
static struct rt_device_pwm *pwm_arg(cJSON *args, AgentToolNode_t node,
                                     const char **name_out, int *channel)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_device_t dev;
    cJSON *item;

    dev = agent_dev_find_class(args, node, name_out, RT_Device_Class_PWM, "pwm", RT_FALSE,
                               out, &used);
    if (!dev) return RT_NULL;
    item = cJSON_GetObjectItemCaseSensitive(args, "channel");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'channel' must be a channel number (0, 1, ...)");
        return RT_NULL;
    }
    *channel = (int)item->valuedouble;
    return (struct rt_device_pwm *)dev;
}

void tool_pwm_set(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    struct rt_device_pwm *pwm;
    cJSON *period = cJSON_GetObjectItemCaseSensitive(args, "period_ns");
    cJSON *pulse = cJSON_GetObjectItemCaseSensitive(args, "pulse_ns");
    long period_ns, pulse_ns;
    int channel = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    pwm = pwm_arg(args, node, &name, &channel);
    if (!pwm) return;
    if (!cJSON_IsNumber(period) || !cJSON_IsNumber(pulse))
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'period_ns' and 'pulse_ns' are both required, in nanoseconds");
        return;
    }
    period_ns = (long)period->valuedouble;
    pulse_ns = (long)pulse->valuedouble;
    if (period_ns <= 0 || pulse_ns < 0 || pulse_ns > period_ns)
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: need period_ns > 0 and 0 <= pulse_ns <= period_ns");
        return;
    }

    result = rt_pwm_set(pwm, channel, (rt_uint32_t)period_ns, (rt_uint32_t)pulse_ns);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: pwm set failed: device=%s channel=%d err=%d", name, channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "pwm set ok: device=%s channel=%d period_ns=%d pulse_ns=%d duty=%d.%d%%",
               name, channel, (int)period_ns, (int)pulse_ns,
               (int)(pulse_ns * 100 / period_ns), (int)(pulse_ns * 1000 / period_ns % 10));
    agent_dev_append(out, sizeof(out), &used,
               "\nnote: the channel is not started yet; call pwm_enable to actually drive the pin");
    agent_dev_finish(node, RT_EOK, out);
}

void tool_pwm_get(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    struct rt_device_pwm *pwm;
    struct rt_pwm_configuration cfg;
    int channel = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    pwm = pwm_arg(args, node, &name, &channel);
    if (!pwm) return;

    rt_memset(&cfg, 0, sizeof(cfg));
    cfg.channel = (rt_uint32_t)channel;
    result = rt_pwm_get(pwm, &cfg);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: pwm get failed: device=%s channel=%d err=%d", name, channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "pwm channel=%d period_ns=%u pulse_ns=%u duty=%d.%d%%",
               (int)cfg.channel, (unsigned)cfg.period, (unsigned)cfg.pulse,
               cfg.period ? (int)(cfg.pulse * 100 / cfg.period) : 0,
               cfg.period ? (int)(cfg.pulse * 1000 / cfg.period % 10) : 0);
    agent_dev_finish(node, RT_EOK, out);
}

static void pwm_switch(cJSON *args, AgentToolNode_t node, rt_bool_t enable)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    struct rt_device_pwm *pwm;
    int channel = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    pwm = pwm_arg(args, node, &name, &channel);
    if (!pwm) return;

    result = enable ? rt_pwm_enable(pwm, channel) : rt_pwm_disable(pwm, channel);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: pwm %s failed: device=%s channel=%d err=%d",
                   enable ? "enable" : "disable", name, channel, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "pwm %s ok: device=%s channel=%d",
               enable ? "enabled" : "disabled", name, channel);
    if (enable)
        agent_dev_append(out, sizeof(out), &used,
                   "\nnote: the output now follows the last pwm_set for this channel");
    agent_dev_finish(node, RT_EOK, out);
}

void tool_pwm_enable(cJSON *args, AgentToolNode_t node) { pwm_switch(args, node, RT_TRUE); }
void tool_pwm_disable(cJSON *args, AgentToolNode_t node) { pwm_switch(args, node, RT_FALSE); }

static void pwm_set_desc(cJSON *func)
{
    tool_desc(func, "Set a PWM channel's period and pulse width in nanoseconds (duty = "
        "pulse/period).");
    tool_desc_state_changing(func);
    tool_desc_more(func, "The channel stays stopped until pwm_enable.");
}

static void pwm_set_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact PWM device name from device_list.");
    create_property(props, "channel", "number", "PWM channel number.");
    create_property(props, "period_ns", "number", "Period in nanoseconds, 1 ns .. 4.29 s.");
    create_property(props, "pulse_ns", "number",
        "Active pulse width in nanoseconds, 0 .. period_ns.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
    cJSON_AddItemToArray(required, cJSON_CreateString("period_ns"));
    cJSON_AddItemToArray(required, cJSON_CreateString("pulse_ns"));
}

static void pwm_get_desc(cJSON *func)
{
    tool_desc(func, "Read back a PWM channel's configured period and pulse width.");
    tool_desc_read_only(func);
}

static void pwm_get_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact PWM device name from device_list.");
    create_property(props, "channel", "number", "PWM channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static void pwm_enable_desc(cJSON *func)
{
    tool_desc(func, "Start a PWM channel that was configured with pwm_set.");
    tool_desc_more(func, "State-changing: the pin begins toggling immediately.");
    tool_desc_more(func, "Call only after the user authorized this peripheral.");
}

static void pwm_enable_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact PWM device name from device_list.");
    create_property(props, "channel", "number", "PWM channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static void pwm_disable_desc(cJSON *func)
{
    tool_desc(func, "Stop a PWM channel.");
    tool_desc_more(func, "State-changing, but the safe direction: the output goes idle.");
}

static void pwm_disable_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "Exact PWM device name from device_list.");
    create_property(props, "channel", "number", "PWM channel number.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("channel"));
}

static const struct agent_tool_def pwm_tools[] =
{
    {
        "pwm_set",
        pwm_set_desc,
        pwm_set_params,
        tool_pwm_set
    },
    {
        "pwm_get",
        pwm_get_desc,
        pwm_get_params,
        tool_pwm_get
    },
    {
        "pwm_enable",
        pwm_enable_desc,
        pwm_enable_params,
        tool_pwm_enable
    },
    {
        "pwm_disable",
        pwm_disable_desc,
        pwm_disable_params,
        tool_pwm_disable
    }
};

AGENT_TOOL_MODULE_EXPORT(pwm, pwm_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_PWM */
