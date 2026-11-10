/*
 * tool_device_i2c.c — I2C 总线工具
 *
 * 3 个工具封装 rt_i2c_master_send/recv 与 rt_i2c_transfer，按总线设备名 + 7/10 位从机地址操作。
 * ops 在总线注册时就已经挂好，所以不需要先 device_open；每次传输的字节数上限沿用框架的
 * DEVICE_IO_MAX。写与组合读写都算写操作，描述里要求用户先授权。
 */
#include "tool_device_internal.h"

#if defined(RT_USING_DEVICE) && defined(PKG_AGENT_TOOL_DEVICE_I2C)

#include <rtdevice.h>
#include <drivers/dev_i2c.h>

#define I2C_ADDR_MAX 0x3ff

/*
 * @brief  解析并校验 I2C 总线设备与从机地址
 * @param  args     工具参数 JSON 对象
 * @param  node     工具节点，失败时用于回填结果
 * @param  name_out 输出总线设备名
 * @param  addr     输出从机地址
 * @return I2C 总线设备，失败返回 RT_NULL
 */
static struct rt_i2c_bus_device *i2c_bus_arg(cJSON *args, AgentToolNode_t node,
                                             const char **name_out, rt_uint16_t *addr)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0;
    rt_device_t dev;
    cJSON *item;

    dev = agent_dev_find_class(args, node, name_out, RT_Device_Class_I2CBUS, "i2cbus", RT_FALSE,
                               out, &used);
    if (!dev) return RT_NULL;
    item = cJSON_GetObjectItemCaseSensitive(args, "addr");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > I2C_ADDR_MAX)
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'addr' must be a 7-bit (0x08..0x77) or 10-bit slave address as a number");
        return RT_NULL;
    }
    *addr = (rt_uint16_t)item->valuedouble;
    return rt_container_of(dev, struct rt_i2c_bus_device, parent);
}

/*
 * @brief  解析并校验 data 与 encoding，把待发送字节写进缓冲区
 * @param  args   工具参数 JSON 对象
 * @param  node   工具节点，失败时用于回填结果
 * @param  buffer 输出字节缓冲区
 * @param  len    输出字节长度
 * @return 成功返回 RT_TRUE，失败返回 RT_FALSE
 */
static rt_bool_t i2c_payload(cJSON *args, AgentToolNode_t node, rt_uint8_t *buffer,
                             rt_size_t *len)
{
    cJSON *data = cJSON_GetObjectItemCaseSensitive(args, "data");
    cJSON *encoding = cJSON_GetObjectItemCaseSensitive(args, "encoding");
    int decoded;

    if (!cJSON_IsString(data) || data->valuestring[0] == '\0')
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'data' must be a non-empty string (text, or hex with encoding='hex')");
        return RT_FALSE;
    }
    if (encoding && !cJSON_IsString(encoding))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'encoding' must be 'text' or 'hex'");
        return RT_FALSE;
    }
    if (encoding && rt_strcmp(encoding->valuestring, "hex") == 0)
    {
        char err[192];

        decoded = agent_dev_hex_decode(data->valuestring, buffer, DEVICE_IO_MAX, err, sizeof(err));
        if (decoded < 0)
        {
            agent_dev_finish(node, -RT_EINVAL, err);
            return RT_FALSE;
        }
        *len = (rt_size_t)decoded;
        if (*len == 0)
        {
            agent_dev_finish(node, -RT_EINVAL, "error: nothing to write after decoding");
            return RT_FALSE;
        }
        return RT_TRUE;
    }
    if (encoding && rt_strcmp(encoding->valuestring, "text") != 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: 'encoding' must be 'text' or 'hex'");
        return RT_FALSE;
    }
    *len = rt_strlen(data->valuestring);
    if (*len > DEVICE_IO_MAX)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: data too large (limit 2048 bytes)");
        return RT_FALSE;
    }
    rt_memcpy(buffer, data->valuestring, *len);
    if (*len == 0)
    {
        agent_dev_finish(node, -RT_EINVAL, "error: nothing to write after decoding");
        return RT_FALSE;
    }
    return RT_TRUE;
}

/*
 * @brief  解析并校验要读取的字节数
 * @param  args   工具参数 JSON 对象
 * @param  node   工具节点，失败时用于回填结果
 * @param  count  输出字节数，缺省为 1
 * @return 成功返回 RT_TRUE，失败返回 RT_FALSE
 */
static rt_bool_t i2c_count_arg(cJSON *args, AgentToolNode_t node, rt_size_t *count)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "count");

    *count = 1;
    if (!item) return RT_TRUE;
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > DEVICE_IO_MAX)
    {
        agent_dev_finish(node, -RT_EINVAL,
               "error: 'count' must be a number of bytes between 1 and 2048");
        return RT_FALSE;
    }
    *count = (rt_size_t)item->valuedouble;
    return RT_TRUE;
}

/*
 * @brief  向一个 I2C 从机写入一段字节
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_write(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0, len = 0;
    const char *name = RT_NULL;
    struct rt_i2c_bus_device *bus;
    rt_uint8_t *buffer;
    rt_uint16_t addr = 0;
    rt_ssize_t sent;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    bus = i2c_bus_arg(args, node, &name, &addr);
    if (!bus) return;
    buffer = (rt_uint8_t *)rt_malloc(DEVICE_IO_MAX);
    if (!buffer)
    {
        agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
        return;
    }
    if (!i2c_payload(args, node, buffer, &len))
    {
        rt_free(buffer);
        return;
    }

    sent = rt_i2c_master_send(bus, addr, RT_I2C_WR, buffer, (rt_uint32_t)len);
    rt_free(buffer);
    if (sent < 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: i2c write failed: bus=%s addr=0x%03x bytes=%d err=%d",
                   name, (int)addr, (int)len, (int)sent);
        agent_dev_finish(node, (rt_err_t)sent, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "i2c write %s: bus=%s addr=0x%03x sent=%d/%d",
               ((rt_size_t)sent == len) ? "ok" : "partial", name, (int)addr, (int)sent, (int)len);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  从一个 I2C 从机读取一段字节
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_read(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0, count = 0;
    const char *name = RT_NULL;
    struct rt_i2c_bus_device *bus;
    rt_uint8_t *buffer;
    rt_uint16_t addr = 0;
    rt_ssize_t got;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    bus = i2c_bus_arg(args, node, &name, &addr);
    if (!bus) return;
    if (!i2c_count_arg(args, node, &count)) return;

    buffer = (rt_uint8_t *)rt_malloc(count);
    if (!buffer)
    {
        agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
        return;
    }
    got = rt_i2c_master_recv(bus, addr, RT_I2C_RD, buffer, (rt_uint32_t)count);
    if (got < 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: i2c read failed: bus=%s addr=0x%03x count=%d err=%d",
                   name, (int)addr, (int)count, (int)got);
        rt_free(buffer);
        agent_dev_finish(node, (rt_err_t)got, out);
        return;
    }
    agent_dev_append(out, sizeof(out), &used,
               "i2c read %s: bus=%s addr=0x%03x got=%d/%d",
               ((rt_size_t)got == count) ? "ok" : "short", name, (int)addr, (int)got, (int)count);
    agent_dev_hex_preview(out, sizeof(out), &used, buffer, (rt_size_t)got);
    rt_free(buffer);
    agent_dev_finish(node, RT_EOK, out);
}

/*
 * @brief  在一次 I2C 传输里先写后读，用于读寄存器
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_writeread(cJSON *args, AgentToolNode_t node)
{
    char out[DEVICE_OUT_SIZE];
    rt_size_t used = 0, wlen = 0, count = 0;
    const char *name = RT_NULL;
    struct rt_i2c_bus_device *bus;
    rt_uint8_t *send_buf, *recv_buf;
    struct rt_i2c_msg msgs[2];
    rt_uint16_t addr = 0;
    rt_ssize_t done;

    if (!node) return;
    agent_dev_reset_result(node);
    if (!cJSON_IsObject(args))
    {
        agent_dev_finish(node, -RT_EINVAL, "error: arguments must be a JSON object");
        return;
    }
    bus = i2c_bus_arg(args, node, &name, &addr);
    if (!bus) return;
    if (!i2c_count_arg(args, node, &count)) return;

    send_buf = (rt_uint8_t *)rt_malloc(DEVICE_IO_MAX);
    recv_buf = (rt_uint8_t *)rt_malloc(count);
    if (!send_buf || !recv_buf)
    {
        if (send_buf) rt_free(send_buf);
        if (recv_buf) rt_free(recv_buf);
        agent_dev_finish(node, -RT_ENOMEM, "error: out of memory");
        return;
    }
    if (!i2c_payload(args, node, send_buf, &wlen))
    {
        rt_free(send_buf);
        rt_free(recv_buf);
        return;
    }

    msgs[0].addr = addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].len = (rt_uint16_t)wlen;
    msgs[0].buf = send_buf;
    msgs[1].addr = addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].len = (rt_uint16_t)count;
    msgs[1].buf = recv_buf;

    done = rt_i2c_transfer(bus, msgs, 2);
    if (done < 0)
    {
        agent_dev_append(out, sizeof(out), &used,
                   "error: i2c write-then-read failed: bus=%s addr=0x%03x err=%d",
                   name, (int)addr, (int)done);
    }
    else
    {
        agent_dev_append(out, sizeof(out), &used,
                   "i2c write-then-read ok: bus=%s addr=0x%03x wrote=%d read=%d "
                   "(repeated start when the adapter supports it)",
                   name, (int)addr, (int)wlen, (int)count);
        agent_dev_hex_preview(out, sizeof(out), &used, recv_buf, count);
    }
    rt_free(send_buf);
    rt_free(recv_buf);
    agent_dev_finish(node, done < 0 ? (rt_err_t)done : RT_EOK, out);
}

/*
 * @brief  填 i2c_write 工具的描述
 * @param  func 描述 JSON 对象
 */
static void i2c_write_desc(cJSON *func)
{
    tool_desc(func, "Send bytes to an I2C slave on a bus device (START, address, data, STOP).");
    tool_desc_more(func, "State-changing: call only after the user explicitly authorized this bus "
        "and address; a returned byte count only proves the transfer completed on the wire.");
}

/*
 * @brief  填 i2c_write 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void i2c_write_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact I2C bus device name from device_list (class i2cbus).");
    create_property(props, "addr", "number",
        "7-bit (or 10-bit) slave address as a number, e.g. 0x68.");
    create_property(props, "data", "string",
        "Bytes to send: plain text, or hex pairs with encoding='hex' (e.g. "
        "'80 00').");
    create_property(props, "encoding", "string", "'text' (default) or 'hex'.");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("addr"));
    cJSON_AddItemToArray(required, cJSON_CreateString("data"));
}

/*
 * @brief  填 i2c_read 工具的描述
 * @param  func 描述 JSON 对象
 */
static void i2c_read_desc(cJSON *func)
{
    tool_desc(func, "Read bytes from an I2C slave on a bus device (START, read address, data, "
        "STOP).");
    tool_desc_read_only(func);
    tool_desc_more(func, "Returns hex and printable text.");
}

/*
 * @brief  填 i2c_read 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void i2c_read_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact I2C bus device name from device_list (class i2cbus).");
    create_property(props, "addr", "number", "7-bit (or 10-bit) slave address as a number.");
    create_property(props, "count", "number", "Bytes to read, 1..2048 (default 1).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("addr"));
}

/*
 * @brief  填 i2c_writeread 工具的描述
 * @param  func 描述 JSON 对象
 */
static void i2c_writeread_desc(cJSON *func)
{
    tool_desc(func, "Write then read in one I2C transfer, the usual way to read a register: send "
        "the register offset, then clock back the value.");
    tool_desc_more(func, "The write half is a state change, so call only after the user "
        "authorized this bus and address.");
}

/*
 * @brief  填 i2c_writeread 工具的参数表
 * @param  props    属性 JSON 对象
 * @param  required 必填项数组
 */
static void i2c_writeread_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact I2C bus device name from device_list (class i2cbus).");
    create_property(props, "addr", "number", "7-bit (or 10-bit) slave address as a number.");
    create_property(props, "data", "string",
        "Bytes to send first, e.g. a register offset as hex with "
        "encoding='hex'.");
    create_property(props, "encoding", "string", "'text' (default) or 'hex'.");
    create_property(props, "count", "number", "Bytes to read back, 1..2048 (default 1).");
    cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    cJSON_AddItemToArray(required, cJSON_CreateString("addr"));
    cJSON_AddItemToArray(required, cJSON_CreateString("data"));
}

static const struct agent_tool_def i2c_tools[] =
{
    {
        "i2c_write",
        i2c_write_desc,
        i2c_write_params,
        tool_i2c_write
    },
    {
        "i2c_read",
        i2c_read_desc,
        i2c_read_params,
        tool_i2c_read
    },
    {
        "i2c_writeread",
        i2c_writeread_desc,
        i2c_writeread_params,
        tool_i2c_writeread
    }
};

AGENT_TOOL_MODULE_EXPORT(i2c, i2c_tools)

#endif /* RT_USING_DEVICE && PKG_AGENT_TOOL_DEVICE_I2C */
