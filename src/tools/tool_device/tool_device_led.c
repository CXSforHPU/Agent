/*
 * tool_device_led.c — LED 工具
 *
 * LED 框架把每个 led 设备注册成字符设备（led0、led1…），文本协议是写 "on"/"off"/"toggle"/
 * "blink" 切状态、写十进制字符串设亮度、读回当前状态名。这里沿用该协议走 rt_device_read/write，
 * 不强转 struct rt_led_device；使用前需要先 device_open。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_LED)

#include <rtdevice.h>
#include <drivers/led.h>

static const char *led_states[] = { "off", "on", "toggle", "blink" };

/*
 * @brief  判断设备名是否是 LED 框架注册的 led + 十进制编号形式
 * @param  name 设备名
 * @return 是 LED 设备名返回 RT_TRUE，否则返回 RT_FALSE
 */
static rt_bool_t led_name_ok(const char *name)
{
    const char *p;

    if (rt_strncmp(name, "led", 3) != 0) return RT_FALSE;
    p = name + 3;
    if (*p == '\0') return RT_FALSE;
    while (*p)
    {
        if (*p < '0' || *p > '9') return RT_FALSE;
        p++;
    }
    return RT_TRUE;
}

/*
 * @brief  查找 LED 设备并校验它属于字符设备类
 * @param  args     工具参数 JSON 对象
 * @param  node     工具节点，失败时用于回填结果
 * @param  name_out 输出设备名
 * @param  out      结果缓冲区
 * @param  used     已用字节数
 * @return LED 设备，失败返回 RT_NULL
 */
static rt_device_t led_arg(cJSON *args, AgentToolNode_t node, const char **name_out, char *out,
                           rt_size_t *used)
{
    rt_device_t dev = agent_dev_find(args, node, name_out, RT_TRUE, out, used);

    if (!dev) return RT_NULL;
    if (dev->type != RT_Device_Class_Char || !led_name_ok(*name_out))
    {
        agent_dev_append(out, DEVICE_OUT_SIZE, used,
                   "error: '%s' is not an LED device; the LED framework registers them as "
                   "'led0', 'led1', ... (class char) — call device_list with class=\"char\"",
                   *name_out);
        agent_dev_finish(node, -RT_EINVAL, out);
        return RT_NULL;
    }
    return dev;
}

/*
 * @brief  设置一个 LED 的开关、切换或闪烁状态
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_set(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *value;
    size_t i;
    rt_bool_t matched = RT_FALSE;
    rt_ssize_t written;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = led_arg(args, node, &name, out, &used);
    if (!dev) return;

    value = cJSON_GetObjectItemCaseSensitive(args, "state");
    if (cJSON_IsString(value))
    {
        for (i = 0; i < sizeof(led_states) / sizeof(led_states[0]); i++)
        {
            if (rt_strcmp(value->valuestring, led_states[i]) == 0)
            {
                matched = RT_TRUE;
                break;
            }
        }
    }
    if (!matched)
    {
        agent_dev_append(out, sizeof(out), &used, "error: 'state' must be 'on','off','toggle' or 'blink'");
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }

    written = rt_device_write(dev, 0, value->valuestring, rt_strlen(value->valuestring));
    if (written < 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: led write failed: device=%s err=%d",
                   name, (int)written);
        agent_dev_finish(node, (rt_err_t)written, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "led %s ok: device=%s", value->valuestring, name);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  设置一个 LED 的亮度等级
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_brightness(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *value;
    char text[16];
    long number;
    rt_size_t len;
    rt_ssize_t written;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = led_arg(args, node, &name, out, &used);
    if (!dev) return;

    value = cJSON_GetObjectItemCaseSensitive(args, "brightness");
    if (cJSON_IsNumber(value))
    {
        number = (long)value->valuedouble;
    }
    else if (!cJSON_IsString(value) || !agent_dev_parse_num(value->valuestring, &number))
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: 'brightness' must be a number in the driver's scale");
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }
    if (number < 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: 'brightness' must not be negative");
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }

    rt_snprintf(text, sizeof(text), "%d", (int)number);
    len = rt_strlen(text);
    written = rt_device_write(dev, 0, text, len);
    if (written < 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: led brightness write failed: device=%s err=%d",
                   name, (int)written);
        agent_dev_finish(node, (rt_err_t)written, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "led brightness: device=%s wanted=%d", name,
               (int)number);
    if ((rt_size_t)written != len)
        agent_dev_append(out, sizeof(out), &used, "\nnote: the driver did not accept the whole value");
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  读回一个 LED 当前的状态名
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_get(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    char text[32];
    rt_ssize_t got;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = led_arg(args, node, &name, out, &used);
    if (!dev) return;

    rt_memset(text, 0, sizeof(text));
    got = rt_device_read(dev, 0, text, sizeof(text) - 1);
    if (got < 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: led read failed: device=%s err=%d",
                   name, (int)got);
        agent_dev_finish(node, (rt_err_t)got, out);
        return;
    }
    text[sizeof(text) - 1] = '\0';
    agent_dev_append(out, sizeof(out), &used, "led %s state=%s", name, text);
    agent_dev_append(out, sizeof(out), &used,
               "\nnote: the LED driver reports the byte count of its fixed-size state word, "
               "the text above is the state name");
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  填 led_set 工具的描述
 * @param  func 描述 JSON 对象
 */
static void led_set_desc(cJSON *func)
{
    tool_desc(func, "Set an LED to 'off', 'on', 'toggle' or 'blink' through the LED framework.");
    tool_desc_state_changing(func);
    tool_desc_more(func, "The device must be opened first with device_open.");
}

/*
 * @brief  填 led_set 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void led_set_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "LED device name from device_list, e.g. 'led0'.");
    create_property(props, "state", "string", "'off', 'on', 'toggle' or 'blink'.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("state"));
}

/*
 * @brief  填 led_get 工具的描述
 * @param  func 描述 JSON 对象
 */
static void led_get_desc(cJSON *func)
{
    tool_desc(func, "Read back an LED device's current state name ('off', 'on', 'blink', ...).");
    tool_desc_read_only(func);
    tool_desc_more(func, "The device must be opened first with device_open.");
}

/*
 * @brief  填 led_get 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void led_get_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "LED device name from device_list, e.g. 'led0'.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

/*
 * @brief  填 led_brightness 工具的描述
 * @param  func 描述 JSON 对象
 */
static void led_brightness_desc(cJSON *func)
{
    tool_desc(func, "Set an LED's brightness level in the driver's own scale (PWM-backed LEDs "
        "usually take a level index, not a percentage).");
    tool_desc_state_changing(func);
    tool_desc_more(func, "The device must be opened first with device_open.");
}

/*
 * @brief  填 led_brightness 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void led_brightness_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string", "LED device name from device_list, e.g. 'led0'.");
    create_property(props, "brightness", "number",
        "Level value accepted by the driver; the range is driver-specific.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("brightness"));
}

static const struct agent_tool_def led_tools[] =
{
    {
        "led_set",
        led_set_desc,
        led_set_params,
        tool_led_set
    },
    {
        "led_get",
        led_get_desc,
        led_get_params,
        tool_led_get
    },
    {
        "led_brightness",
        led_brightness_desc,
        led_brightness_params,
        tool_led_brightness
    }
};

AGENT_TOOL_MODULE_EXPORT(led, led_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_LED */
