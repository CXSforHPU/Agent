/*
 * tool_device_sensor.c — 传感器工具
 *
 * 3 个工具：sensor_read 走 rt_device_read 取 struct rt_sensor_data 并按 data.type 解码成
 * 带单位的读数；sensor_info 走 RT_SENSOR_CTRL_GET_INFO；sensor_control 只暴露框架里能用
 * 标量参数表达的几条命令。读取需要先 device_open（与内核 read 通路一致）。
 */
#include "tool_device_internal.h"

/* 传感器框架 v2 换掉了 struct rt_sensor_data 与命令字（改成 frame/buffer 模型），
 * 本模块对接的是 v1；开了 RT_USING_SENSOR_V2 的工程这里整文件不参与编译。 */
#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_SENSOR) && !defined(RT_USING_SENSOR_V2)

#include <rtdevice.h>
#include <drivers/sensor.h>

struct sensor_class
{
    const char *name;
    const char *unit;
    rt_bool_t three_axis;
};

/* 下标与 sensor.h 的 RT_SENSOR_CLASS_* 对齐。 */
static const struct sensor_class sensor_classes[] =
{
    { "none",        "1",       RT_FALSE },
    { "accel",       "mG",      RT_TRUE  },
    { "gyro",        "mdps",    RT_TRUE  },
    { "mag",         "mGauss",  RT_TRUE  },
    { "temperature", "dCelsius",RT_FALSE },
    { "humidity",    "permille",RT_FALSE },
    { "pressure",    "Pa",      RT_FALSE },
    { "light",       "lux",     RT_FALSE },
    { "proximity",   "cm",      RT_FALSE },
    { "heart_rate",  "bpm",     RT_FALSE },
    { "tvoc",        "permille",RT_FALSE },
    { "noise",       "Hz",      RT_FALSE },
    { "step",        "1",       RT_FALSE },
    { "force",       "mN",      RT_FALSE },
    { "dust",        "ug/m3",   RT_FALSE },
    { "eco2",        "ppm",     RT_FALSE },
    { "coordinates", "degrees", RT_FALSE },
    { "tof",         "mm",      RT_FALSE },
    { "spo2",        "permille",RT_FALSE },
    { "iaq",         "1",       RT_FALSE },
    { "etoh",        "ppm",     RT_FALSE },
    { "blood_pressure","mmHg",  RT_FALSE },
    { "voltage",     "mV",      RT_FALSE },
    { "current",     "mA",      RT_FALSE },
    { "power",       "mW",      RT_FALSE },
};

/*
 * @brief  取传感器单位枚举对应的单位文本
 * @param  unit 单位枚举值
 * @return 单位文本，未知单位返回 "unknown"
 */
static const char *sensor_unit_name(rt_uint8_t unit)
{
    static const char *names[] =
    {
        "none", "mG", "mdps", "mGauss", "lux", "cm", "Pa", "permille", "dCelsius", "Hz",
        "1", "bpm", "mm", "mN", "ppm", "ppb", "DMS", "DD", "mg/m3", "mmHg", "mV", "mA", "mW",
    };

    if (unit < sizeof(names) / sizeof(names[0])) return names[unit];
    return "unknown";
}

/*
 * @brief  查找传感器设备并校验它属于传感器类
 * @param  args      工具参数 JSON 对象
 * @param  node      工具节点，失败时用于回填结果
 * @param  name_out  输出设备名
 * @param  need_open 是否要求设备处于已打开状态
 * @return 传感器设备，失败返回 RT_NULL
 */
static rt_device_t sensor_arg(cJSON *args, AgentToolNode_t node, const char **name_out,
                              rt_bool_t need_open)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;

    return agent_dev_find_class(args, node, name_out, RT_Device_Class_Sensor, "sensor",
                                need_open, out, &used);
}

/*
 * @brief  按 data.type 解码采样值并带上框架定义的单位
 * @param  out   结果缓冲区
 * @param  size  结果缓冲区大小
 * @param  used  已用字节数
 * @param  d     一条采样数据
 */
static void sensor_decode(char *out, rt_size_t size, rt_size_t *used,
                          const struct rt_sensor_data *d)
{
    if (d->type >= sizeof(sensor_classes) / sizeof(sensor_classes[0]))
    {
        agent_dev_append(out, size, used, "\nraw: type=%d value=%d", (int)d->type,
                   (int)d->data.tvoc);
        return;
    }
    if (sensor_classes[d->type].three_axis)
    {
        agent_dev_append(out, size, used, "\n%s: x=%d y=%d z=%d %s",
                   sensor_classes[d->type].name, (int)d->data.acce.x, (int)d->data.acce.y,
                   (int)d->data.acce.z, sensor_classes[d->type].unit);
        return;
    }
    switch (d->type)
    {
    case RT_SENSOR_CLASS_TEMP:
        agent_dev_append(out, size, used, "\ntemperature: %d.%d Celsius",
                   (int)(d->data.temp / 10), (int)(d->data.temp < 0 ? -(d->data.temp % 10)
                                                                    : d->data.temp % 10));
        break;
    case RT_SENSOR_CLASS_GNSS:
        agent_dev_append(out, size, used, "\ncoordinates: lon_microdeg=%d lat_microdeg=%d",
                   (int)(d->data.coord.longitude * 1e6), (int)(d->data.coord.latitude * 1e6));
        break;
    case RT_SENSOR_CLASS_BP:
        agent_dev_append(out, size, used, "\nblood_pressure: sbp=%d dbp=%d mmHg",
                   (int)d->data.bp.sbp, (int)d->data.bp.dbp);
        break;
    case RT_SENSOR_CLASS_VOLTAGE:
        agent_dev_append(out, size, used, "\nvoltage: %d mV", (int)d->data.mv);
        break;
    case RT_SENSOR_CLASS_CURRENT:
        agent_dev_append(out, size, used, "\ncurrent: %d mA", (int)d->data.ma);
        break;
    case RT_SENSOR_CLASS_POWER:
        agent_dev_append(out, size, used, "\npower: %d mW", (int)d->data.mw);
        break;
    default:
        agent_dev_append(out, size, used, "\n%s: %d %s", sensor_classes[d->type].name,
                   (int)d->data.tvoc, sensor_classes[d->type].unit);
        break;
    }
}

/*
 * @brief  读取一次传感器数据并解码成带单位的读数
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_read(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    struct rt_sensor_data data;
    rt_size_t got;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = sensor_arg(args, node, &name, RT_TRUE);
    if (!dev) return;

    rt_memset(&data, 0, sizeof(data));
    got = rt_device_read(dev, 0, &data, sizeof(data));
    if (got != sizeof(data))
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: sensor read failed: device=%s got=%d of %d bytes "
                   "(open with rdonly or rdwr first)",
                   name, (int)got, (int)sizeof(data));
        agent_dev_finish(node, -RT_ERROR, out);
        return;
    }

    agent_dev_append(out, sizeof(out), &used, "sensor=%s timestamp=%u", name,
               (unsigned)data.timestamp);
    sensor_decode(out, sizeof(out), &used, &data);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  显示一个传感器的型号、类、厂商、单位与量程
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_info(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    struct rt_sensor_info info;
    rt_uint32_t id = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = sensor_arg(args, node, &name, RT_FALSE);
    if (!dev) return;

    rt_memset(&info, 0, sizeof(info));
    result = rt_device_control(dev, RT_SENSOR_CTRL_GET_INFO, &info);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: GET_INFO failed: device=%s err=%d", name, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "sensor=%s model=%s class=%d vendor=%d unit=%s range=%d..%d min_period_ms=%u fifo=%d",
               name, info.model ? info.model : "-", (int)info.type, (int)info.vendor,
               sensor_unit_name(info.unit), (int)info.range_min, (int)info.range_max,
               (unsigned)info.period_min, (int)info.fifo_max);
    if (rt_device_control(dev, RT_SENSOR_CTRL_GET_ID, &id) == RT_EOK)
        agent_dev_append(out, sizeof(out), &used, "\nchip_id=0x%02x", (unsigned)id);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  执行一条可带标量参数的传感器控制命令
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_control(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    rt_device_t dev;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "cmd");
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(args, "arg");
    static const struct
    {
        const char *name;
        int value;
        rt_bool_t needs_arg;
    } cmds[] =
    {
        { "SET_RANGE", RT_SENSOR_CTRL_SET_RANGE, RT_TRUE  },
        { "SET_ODR",   RT_SENSOR_CTRL_SET_ODR,   RT_TRUE  },
        { "SET_MODE",  RT_SENSOR_CTRL_SET_MODE,  RT_TRUE  },
        { "SET_POWER", RT_SENSOR_CTRL_SET_POWER, RT_TRUE  },
        { "SELF_TEST", RT_SENSOR_CTRL_SELF_TEST, RT_FALSE },
    };
    int cmd_value = 0;
    rt_int32_t cmd_arg = 0;
    size_t i;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    dev = sensor_arg(args, node, &name, RT_FALSE);
    if (!dev) return;
    if (!cJSON_IsString(cmd))
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'cmd' must be 'SET_RANGE','SET_ODR','SET_MODE','SET_POWER' or 'SELF_TEST'");
        return;
    }
    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        if (rt_strcmp(cmd->valuestring, cmds[i].name) == 0)
        {
            cmd_value = cmds[i].value;
            break;
        }
    }
    if (cmd_value == 0)
    {
        agent_dev_append(out, sizeof(out), &used, "error: unknown sensor command '%s'",
                   cmd->valuestring);
        agent_dev_finish(node, -RT_EINVAL, out);
        return;
    }
    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        if (cmds[i].value != cmd_value) continue;
        if (cmds[i].needs_arg && !cJSON_IsNumber(arg))
        {
            agent_dev_append(out, sizeof(out), &used,
                       "error: '%s' needs a numeric 'arg'", cmds[i].name);
            agent_dev_finish(node, -RT_EINVAL, out);
            return;
        }
        cmd_arg = cJSON_IsNumber(arg) ? (rt_int32_t)arg->valuedouble : 0;
        break;
    }

    result = rt_device_control(dev, cmd_value, cmd_arg ? &cmd_arg : RT_NULL);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used, "error: %s failed: device=%s err=%d",
                   cmd->valuestring, name, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used, "sensor %s ok: device=%s", cmd->valuestring, name);
    if (cmd_arg) agent_dev_append(out, sizeof(out), &used, " arg=%d", (int)cmd_arg);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  填 sensor_read 工具的描述
 * @param  func 描述 JSON 对象
 */
static void sensor_read_desc(cJSON *func)
{
    tool_desc(func, "Take one reading from a sensor device and decode it into engineering units.");
    tool_desc_more(func, "Open the device first with device_open (mode rdonly is enough).");
    tool_desc_more(func, "Read-only; reports the timestamp the driver stamped and the values as "
        "delivered.");
}

/*
 * @brief  填 sensor_read 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void sensor_read_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact sensor device name from device_list (class sensor).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

/*
 * @brief  填 sensor_info 工具的描述
 * @param  func 描述 JSON 对象
 */
static void sensor_info_desc(cJSON *func)
{
    tool_desc(func, "Show a sensor's model, class, vendor, unit, measurement range, minimum "
        "sampling period and chip id.");
    tool_desc_more(func, "Read-only; use it before changing range or output rate.");
}

/*
 * @brief  填 sensor_info 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void sensor_info_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact sensor device name from device_list (class sensor).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

/*
 * @brief  填 sensor_control 工具的描述
 * @param  func 描述 JSON 对象
 */
static void sensor_control_desc(cJSON *func)
{
    tool_desc(func, "Apply one of the sensor framework commands that take a single scalar "
        "argument: SET_RANGE (measure range, unit per the driver), SET_ODR (output data rate in "
        "Hz), SET_MODE (1=polling, 2=interrupt, 3=fifo), SET_POWER (1=down, 2=normal, 3=low, "
        "4=high), or SELF_TEST.");
    tool_desc_more(func, "State-changing: call only after the user authorized this sensor.");
}

/*
 * @brief  填 sensor_control 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void sensor_control_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact sensor device name from device_list (class sensor).");
    create_property(props, "cmd", "string",
        "'SET_RANGE','SET_ODR','SET_MODE','SET_POWER' or 'SELF_TEST'.");
    create_property(props, "arg", "number",
        "Value for the three-letter commands; omit for SELF_TEST.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("cmd"));
}

static const struct agent_tool_def sensor_tools[] =
{
    {
        "sensor_read",
        sensor_read_desc,
        sensor_read_params,
        tool_sensor_read
    },
    {
        "sensor_info",
        sensor_info_desc,
        sensor_info_params,
        tool_sensor_info
    },
    {
        "sensor_control",
        sensor_control_desc,
        sensor_control_params,
        tool_sensor_control
    }
};

AGENT_TOOL_MODULE_EXPORT(sensor, sensor_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_SENSOR && !RT_USING_SENSOR_V2 */
