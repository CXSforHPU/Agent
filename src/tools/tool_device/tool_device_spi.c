/*
 * tool_device_spi.c — SPI 从机工具
 *
 * 2 个工具封装 rt_spi_configure 与 rt_spi_send_then_recv，参数名沿用设备框架里 SPI 的
 * "从机设备"（spi10 这种挂在总线下的设备），不是裸总线名。
 * SPI 走 bus/ops 通路，不需要先 device_open。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_SPI)

#include <rtdevice.h>
#include <drivers/dev_spi.h>

/*
 * @brief  查找 SPI 从机设备并校验它属于从机设备类
 * @param  args     工具参数 JSON 对象
 * @param  node     工具节点，失败时用于回填结果
 * @param  name_out 输出设备名
 * @return SPI 从机设备，失败返回 RT_NULL
 */
static struct rt_spi_device *spi_arg(cJSON *args, AgentToolNode_t node, const char **name_out)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_device_t dev = agent_dev_find(args, node, name_out, RT_FALSE, out, &used);

    if (!dev) return RT_NULL;
    if (dev->type == RT_Device_Class_SPIBUS)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: '%s' is an SPI bus; these tools need a slave device attached to it "
                   "(e.g. 'spi10'); call device_list with class=\"spidevice\"", *name_out);
        agent_dev_finish(node, -RT_EINVAL, out);
        return RT_NULL;
    }
    if (dev->type != RT_Device_Class_SPIDevice)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: device '%s' is a %s device, this tool needs an SPI slave device",
                   *name_out, agent_dev_class_name(dev->type));
        agent_dev_finish(node, -RT_EINVAL, out);
        return RT_NULL;
    }
    return (struct rt_spi_device *)dev;
}

/*
 * @brief  配置一个 SPI 从机设备的工作模式、时钟、位宽与位序
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_spi_configure(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    const char *name = RT_NULL;
    struct rt_spi_device *spi;
    struct rt_spi_configuration cfg;
    cJSON *item;
    long mode = 0;
    int have_mode = 0, have_hz = 0, have_width = 0;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    spi = spi_arg(args, node, &name);
    if (!spi) return;

    rt_memset(&cfg, 0, sizeof(cfg));
    item = cJSON_GetObjectItemCaseSensitive(args, "mode");
    if (item)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 3)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'mode' must be the SPI mode 0, 1, 2 or 3");
            return;
        }
        mode = (long)item->valuedouble;
        have_mode = 1;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "max_hz");
    if (item)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble < 1)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'max_hz' must be a positive clock in hertz");
            return;
        }
        cfg.max_hz = (rt_uint32_t)item->valuedouble;
        have_hz = 1;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "data_width");
    if (item)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > 32)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'data_width' must be 1..32 bits");
            return;
        }
        cfg.data_width = (rt_uint8_t)item->valuedouble;
        have_width = 1;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "first_bit");
    if (item)
    {
        if (!cJSON_IsString(item) ||
            (rt_strcmp(item->valuestring, "msb") != 0 && rt_strcmp(item->valuestring, "lsb") != 0))
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'first_bit' must be 'msb' or 'lsb'");
            return;
        }
        cfg.mode |= (rt_strcmp(item->valuestring, "msb") == 0) ? RT_SPI_MSB : RT_SPI_LSB;
    }
    if (!have_mode && !have_hz && !have_width && !cJSON_GetObjectItemCaseSensitive(args, "first_bit"))
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: give at least one of 'mode','max_hz','data_width','first_bit'");
        return;
    }
    if (have_mode)
    {
        static const rt_uint8_t modes[] = { RT_SPI_MODE_0, RT_SPI_MODE_1, RT_SPI_MODE_2, RT_SPI_MODE_3 };

        cfg.mode = (cfg.mode & RT_SPI_MSB) | modes[mode];
    }

    result = rt_spi_configure(spi, &cfg);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used, "error: spi configure failed: device=%s err=%d",
                   name, (int)result);
        agent_dev_finish(node, result, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "spi configure ok: device=%s mode=%d max_hz=%u data_width=%d bit_first=%s",
               name, (int)mode, (unsigned)cfg.max_hz, (int)cfg.data_width,
               (cfg.mode & RT_SPI_MSB) ? "msb" : "lsb");
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  在一个 SPI 从机设备上做全双工收发
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_spi_transfer(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_size_t wlen = 0, count = 1;
    const char *name = RT_NULL;
    struct rt_spi_device *spi;
    rt_uint8_t *send_buf, *recv_buf = RT_NULL;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(args, "data");
    cJSON *encoding = cJSON_GetObjectItemCaseSensitive(args, "encoding");
    cJSON *item;
    int decoded;
    rt_err_t result;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    spi = spi_arg(args, node, &name);
    if (!spi) return;
    if (!cJSON_IsString(data) || data->valuestring[0] == '\0')
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'data' must be a non-empty string (text, or hex with encoding='hex')");
        return;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "recv_count");
    if (item)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > DEVICE_IO_MAX)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: 'recv_count' must be 0..2048 bytes");
            return;
        }
        count = (rt_size_t)item->valuedouble;
    }

    send_buf = (rt_uint8_t *)rt_malloc(DEVICE_IO_MAX);
    if (!send_buf)
    {
        agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
        return;
    }
    if (encoding && (!cJSON_IsString(encoding) ||
                     (rt_strcmp(encoding->valuestring, "text") != 0 &&
                      rt_strcmp(encoding->valuestring, "hex") != 0)))
    {
        rt_free(send_buf);
        agent_dev_finish(node, -RT_EINVAL, "error: 'encoding' must be 'text' or 'hex'");
        return;
    }
    if (encoding && rt_strcmp(encoding->valuestring, "hex") == 0)
    {
        decoded = agent_dev_hex_decode(data->valuestring, send_buf, DEVICE_IO_MAX, out, sizeof(out));
        if (decoded < 0)
        {
            rt_free(send_buf);
            agent_dev_finish(node, -RT_EINVAL, out);
            return;
        }
        wlen = (rt_size_t)decoded;
    }
    else
    {
        wlen = rt_strlen(data->valuestring);
        if (wlen > DEVICE_IO_MAX)
        {
            rt_free(send_buf);
            agent_dev_finish(node, -RT_EINVAL, "error: data too large (limit 2048 bytes)");
            return;
        }
        rt_memcpy(send_buf, data->valuestring, wlen);
    }
    if (wlen == 0)
    {
        rt_free(send_buf);
        agent_dev_finish(node, -RT_EINVAL, "error: nothing to send after decoding");
        return;
    }
    if (count > 0)
    {
        recv_buf = (rt_uint8_t *)rt_malloc(count);
        if (!recv_buf)
        {
            rt_free(send_buf);
            agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
            return;
        }
    }

    result = rt_spi_send_then_recv(spi, send_buf, wlen, recv_buf, count);
    if (result != RT_EOK)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: spi transfer failed: device=%s sent=%d recv=%d err=%d",
                   name, (int)wlen, (int)count, (int)result);
    }
    else
    {
        agent_dev_append(out, sizeof(out), &used,
                   "spi transfer ok: device=%s sent=%d recv=%d", name, (int)wlen, (int)count);
        if (recv_buf) agent_dev_hex_preview(out, sizeof(out), &used, recv_buf, count);
    }
    if (recv_buf) rt_free(recv_buf);
    rt_free(send_buf);
    agent_dev_finish(node, result, out);
}

/*
 * @brief  填 spi_configure 工具的描述
 * @param  func 描述 JSON 对象
 */
static void spi_configure_desc(cJSON *func)
{
    tool_desc(func, "Set the SPI settings of one slave device (clock polarity/phase mode, max "
        "clock, bit width, bit order).");
    tool_desc_more(func, "State-changing for that device's next transfers.");
    tool_desc_more(func, "The name must be a slave device attached to a bus, e.g.");
    tool_desc_more(func, "'spi10'.");
}

/*
 * @brief  填 spi_configure 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void spi_configure_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact SPI slave device name from device_list (class spidevice).");
    create_property(props, "mode", "number", "SPI mode 0..3 (CPOL/CPHA combination).");
    create_property(props, "max_hz", "number", "Clock limit in hertz, e.g. 1000000.");
    create_property(props, "data_width", "number", "Bits per transfer word, 1..32.");
    create_property(props, "first_bit", "string", "'msb' (default for most slaves) or 'lsb'.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
}

/*
 * @brief  填 spi_transfer 工具的描述
 * @param  func 描述 JSON 对象
 */
static void spi_transfer_desc(cJSON *func)
{
    tool_desc(func, "Full-duplex exchange on one SPI slave device: clock out 'data' while "
        "clocking back 'recv_count' bytes, chip select asserted for the whole transfer.");
    tool_desc_more(func, "Returns hex and text for what came back.");
    tool_desc_more(func, "The write half is a state change, so call only after the user "
        "authorized this device.");
}

/*
 * @brief  填 spi_transfer 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void spi_transfer_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact SPI slave device name from device_list (class spidevice).");
    create_property(props, "data", "string",
        "Bytes to clock out: plain text, or hex pairs with encoding='hex'.");
    create_property(props, "encoding", "string", "'text' (default) or 'hex'.");
    create_property(props, "recv_count", "number",
        "Bytes to capture at the same time, 0..2048 (default 1).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("data"));
}

static const struct agent_tool_def spi_tools[] =
{
    {
        "spi_configure",
        spi_configure_desc,
        spi_configure_params,
        tool_spi_configure
    },
    {
        "spi_transfer",
        spi_transfer_desc,
        spi_transfer_params,
        tool_spi_transfer
    }
};

AGENT_TOOL_MODULE_EXPORT(spi, spi_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_SPI */
