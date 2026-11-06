/*
 * tool_device_pin.c — GPIO 引脚工具
 *
 * 3 个工具封装 rt_pin_mode/rt_pin_write/rt_pin_read，按全局引脚编号操作。
 * rt_pin_* 直接用注册到框架里的引脚 ops，未注册时会在内核里断言，所以每个工具
 * 先确认存在 RT_Device_Class_Pin 设备再调用。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_PIN)

#include <rtdevice.h>
#include <drivers/dev_pin.h>

static const struct
{
    const char *name;
    rt_uint8_t mode;
} pin_modes[] =
{
    {"output",          PIN_MODE_OUTPUT},
    {"input",           PIN_MODE_INPUT},
    {"input_pullup",    PIN_MODE_INPUT_PULLUP},
    {"input_pulldown",  PIN_MODE_INPUT_PULLDOWN},
    {"output_od",       PIN_MODE_OUTPUT_OD},
};

static void pin_present_cb(rt_device_t dev, void *arg)
{
    if (dev->type == RT_Device_Class_Pin) (*(int *)arg)++;
}

static int pin_device_count(void)
{
    int count = 0;

    agent_dev_foreach(pin_present_cb, &count);
    return count;
}

/* 'pin' 支持编号（35）和引脚名（"PA.0"，交给 rt_pin_get 解析）。 */
static rt_bool_t pin_arg(cJSON *args, AgentToolNode_t node, rt_base_t *pin)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "pin");
    long value;

    if (cJSON_IsNumber(item))
    {
        if (item->valuedouble < 0) goto bad;
        *pin = (rt_base_t)item->valuedouble;
        return RT_TRUE;
    }
    if (cJSON_IsString(item) && agent_dev_parse_num(item->valuestring, &value) && value >= 0)
    {
        *pin = (rt_base_t)value;
        return RT_TRUE;
    }
    if (cJSON_IsString(item) && item->valuestring[0] != '\0')
    {
        value = rt_pin_get(item->valuestring);
        if (value < 0) goto bad;
        *pin = (rt_base_t)value;
        return RT_TRUE;
    }
bad:
    agent_dev_finish(node, -RT_EINVAL,
                     "error: 'pin' must be a pin number or a name such as \"PA.0\"; "
                     "the numbering is set by this BSP's pin driver");
    return RT_FALSE;
}

static rt_bool_t pin_framework(AgentToolNode_t node)
{
    if (pin_device_count() > 0) return RT_TRUE;
    agent_dev_finish(node, -RT_ERROR,
                     "error: no pin driver is registered in the device framework; "
                     "call device_list with class=\"pin\" to check");
    return RT_FALSE;
}

void tool_pin_mode(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(args, "mode");
    long value;
    rt_base_t pin = 0;
    rt_uint8_t pin_mode = 0;
    size_t i;
    int named = 0;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    if (!pin_framework(node) || !pin_arg(args, node, &pin)) return;
    if (cJSON_IsNumber(mode))
    {
        value = (long)mode->valuedouble;
        if (value < 0 || value > 4)
        {
            agent_dev_finish(node, -RT_EINVAL,
                             "error: 'mode' number must be 0..4, see the named modes");
            return;
        }
        pin_mode = (rt_uint8_t)value;
        named = 1;
    }
    else if (cJSON_IsString(mode))
    {
        if (agent_dev_parse_num(mode->valuestring, &value) && value >= 0 && value <= 4)
        {
            pin_mode = (rt_uint8_t)value;
            named = 1;
        }
        else
        {
            for (i = 0; i < sizeof(pin_modes) / sizeof(pin_modes[0]); i++)
            {
                if (rt_strcmp(mode->valuestring, pin_modes[i].name) == 0)
                {
                    pin_mode = pin_modes[i].mode;
                    named = 1;
                    break;
                }
            }
        }
    }
    if (!named)
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'mode' must be 'output','input','input_pullup','input_pulldown',"
               "'output_od' or 0..4");
        return;
    }

    rt_pin_mode(pin, pin_mode);
    agent_dev_append(out, sizeof(out), &used, "pin %d mode set to %d", (int)pin, (int)pin_mode);
    agent_dev_finish(node, RT_EOK, out);
}

void tool_pin_write(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(args, "value");
    long level;
    rt_base_t pin = 0;
    rt_ssize_t written;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    if (!pin_framework(node) || !pin_arg(args, node, &pin)) return;
    if (cJSON_IsNumber(value))
    {
        level = (long)value->valuedouble;
    }
    else if (cJSON_IsString(value) && agent_dev_parse_num(value->valuestring, &level))
    {
        /* numeric string level */
    }
    else if (cJSON_IsString(value) && rt_strcmp(value->valuestring, "high") == 0)
    {
        level = PIN_HIGH;
    }
    else if (cJSON_IsString(value) && rt_strcmp(value->valuestring, "low") == 0)
    {
        level = PIN_LOW;
    }
    else
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'value' must be 0, 1, 'low' or 'high'");
        return;
    }
    if (level < 0 || level > 1)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'value' must be 0 (low) or 1 (high)");
        return;
    }

    rt_pin_write(pin, (rt_ssize_t)level);
    written = rt_pin_read(pin);
    agent_dev_append(out, sizeof(out), &used,
               "pin %d written %s; read back %d", (int)pin, level ? "high" : "low",
               (int)written);
    agent_dev_append(out, sizeof(out), &used,
               "\nnote: a read back of the driven level only proves the output path; an input "
               "pin or an open-drain pin can differ");
    agent_dev_finish(node, RT_EOK, out);
}

void tool_pin_read(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_base_t pin = 0;
    rt_ssize_t level;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    if (!pin_framework(node) || !pin_arg(args, node, &pin)) return;

    level = rt_pin_read(pin);
    agent_dev_append(out, sizeof(out), &used, "pin %d level=%d (%s)",
               (int)pin, (int)level, level ? "high" : "low");
    agent_dev_append(out, sizeof(out), &used,
               "\nnote: set the pin to an input mode with pin_mode before trusting this value");
    agent_dev_finish(node, RT_EOK, out);
}

static void pin_mode_desc(cJSON *func)
{
    tool_desc(func, "Set one GPIO pin's direction and pull configuration through the RT-Thread "
        "pin driver.");
    tool_desc_more(func, "Modes: 'output', 'input', 'input_pullup', 'input_pulldown', "
        "'output_od'.");
    tool_desc_more(func, "The pin numbering follows this BSP's pin driver, so confirm it before "
        "driving hardware.");
}

static void pin_mode_params(cJSON *props, cJSON *required)
{
    create_property(props, "pin", "number",
        "Pin number used by this BSP's pin driver (e.g. 35). A pin name "
        "such as \"PA.0\" also works.");
    create_property(props, "mode", "string",
        "'output', 'input', 'input_pullup', 'input_pulldown', 'output_od', "
        "or 0..4.");
    cJSON_AddItemToArray(required, cJSON_CreateString("pin"));
    cJSON_AddItemToArray(required, cJSON_CreateString("mode"));
}

static void pin_write_desc(cJSON *func)
{
    tool_desc(func, "Drive one GPIO pin high or low.");
    tool_desc_more(func, "Call only after the user explicitly authorized touching this pin; the "
        "tool reports the level read back, which is not proof the hardware acted on it.");
}

static void pin_write_params(cJSON *props, cJSON *required)
{
    create_property(props, "pin", "number", "Pin number used by this BSP's pin driver.");
    create_property(props, "value", "number", "1/'high' or 0/'low'.");
    cJSON_AddItemToArray(required, cJSON_CreateString("pin"));
    cJSON_AddItemToArray(required, cJSON_CreateString("value"));
}

static void pin_read_desc(cJSON *func)
{
    tool_desc(func, "Read the current level of one GPIO pin.");
    tool_desc_read_only(func);
    tool_desc_more(func, "Configure the pin as an input first with pin_mode, otherwise the value "
        "is whatever the output driver is holding.");
}

static void pin_read_params(cJSON *props, cJSON *required)
{
    create_property(props, "pin", "number", "Pin number used by this BSP's pin driver.");
    cJSON_AddItemToArray(required, cJSON_CreateString("pin"));
}

static const struct agent_tool_def pin_tools[] =
{
    {
        "pin_mode",
        pin_mode_desc,
        pin_mode_params,
        tool_pin_mode
    },
    {
        "pin_write",
        pin_write_desc,
        pin_write_params,
        tool_pin_write
    },
    {
        "pin_read",
        pin_read_desc,
        pin_read_params,
        tool_pin_read
    }
};

AGENT_TOOL_MODULE_EXPORT(pin, pin_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_PIN */
