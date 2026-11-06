/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-06     CXSforHPU    first version
 */

/**
 * Test Case Name: Agent Device Framework Tools Test
 *
 * Test Objectives:
 * - Validate the agent `device_*` tools that wrap the RT-Thread device
 *   framework (`rt_device_*`) so the model can drive peripherals without
 *   driver-specific knowledge
 * - Test device discovery (`device_list`, `device_info`), the reference-counted
 *   open/close lifecycle, char and block read/write semantics, and
 *   `device_control` command dispatch (typed, no-arg, refused)
 * - Verify the safety guards: console device is never read or closed, writes
 *   need write access, and operation on closed devices is rejected
 *
 * Test Scenarios:
 * - Register virtual char/block/rtc devices and inspect them alongside the
 *   real console uart through the registered tools
 * - Round-trip text and hex payloads on a char device; write/read whole blocks
 *   on a block device with geometry probing
 * - Run typed commands (BLK_GETGEOME, RTC_GET/SET_TIME), no-arg raw commands,
 *   and check that struct-argument commands are refused with clear errors
 * - Exercise error paths: unknown device, empty name, bad flags, bad hex,
 *   invalid arguments, and the console guards
 * - Load the `device` and `device-commands` skills through the registered
 *   skill tool and compare them with the embedded registry
 *
 * Verification Metrics:
 * - All uassert assertions pass without failure
 * - tool results contain the expected texts and return codes
 * - reference counts return to zero after each lifecycle
 *
 * Dependencies:
 * - Agent package with device tools (`RT_USING_DEVICE`), skill module enabled
 * - Run with: utest_run -thread agent.device_tools
 *
 * Expected Results:
 * - "[  PASSED  ] [ result   ] testcase (agent.device_tools)"
 */

#include <rtthread.h>
#include <time.h>
#include <rtdevice.h>
#include <drivers/dev_rtc.h>
#include "utest.h"
#include "utest_assert.h"
#include "tool_func.h"
#include "utils/tool_json.h"
#include "skill.h"

#define UTST0_BUF_SIZE 256
#define UTBLK_SECTORS  16
#define UTBLK_BPS      512

static rt_uint8_t utst0_buf[UTST0_BUF_SIZE];
static rt_size_t utst0_len;
static rt_uint8_t utblk_ram[UTBLK_SECTORS * UTBLK_BPS];
static time_t utrtc_time = 1700000000;
static rt_bool_t devices_registered = RT_FALSE;
static char args_buf[700];

static rt_err_t utst_init(rt_device_t dev) { (void)dev; return RT_EOK; }

/* Real drivers record the access mode in open_flag (dev_serial.c); the
 * read-only/write-only guards in tool_device rely on that convention. */
static rt_err_t utst_open(rt_device_t dev, rt_uint16_t oflag)
{
    dev->open_flag = oflag & 0xff;
    return RT_EOK;
}
static rt_err_t utst_close(rt_device_t dev) { (void)dev; return RT_EOK; }

static rt_ssize_t utst0_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_size_t n;

    (void)dev; (void)pos;
    n = size < utst0_len ? size : utst0_len;
    rt_memcpy(buffer, utst0_buf, n);
    utst0_len -= n;
    if (n) rt_memmove(utst0_buf, utst0_buf + n, utst0_len);
    return (rt_ssize_t)n;
}

static rt_ssize_t utst0_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_size_t n;

    (void)dev; (void)pos;
    n = UTST0_BUF_SIZE - utst0_len;
    if (n > size) n = size;
    rt_memcpy(utst0_buf + utst0_len, buffer, n);
    utst0_len += n;
    return (rt_ssize_t)n;
}

static rt_err_t utst0_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev; (void)args;
    return (cmd == 0x99) ? RT_EOK : -RT_EINVAL;
}

static rt_ssize_t utblk_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_size_t n;

    (void)dev;
    if (pos < 0 || (rt_size_t)pos >= UTBLK_SECTORS) return -RT_ERROR;
    n = UTBLK_SECTORS - (rt_size_t)pos;
    if (n > size) n = size;
    rt_memcpy(buffer, utblk_ram + (rt_size_t)pos * UTBLK_BPS, n * UTBLK_BPS);
    return (rt_ssize_t)n;
}

static rt_ssize_t utblk_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_size_t n;

    (void)dev;
    if (pos < 0 || (rt_size_t)pos >= UTBLK_SECTORS) return -RT_ERROR;
    n = UTBLK_SECTORS - (rt_size_t)pos;
    if (n > size) n = size;
    rt_memcpy(utblk_ram + (rt_size_t)pos * UTBLK_BPS, buffer, n * UTBLK_BPS);
    return (rt_ssize_t)n;
}

static rt_err_t utblk_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev;
    if (cmd == RT_DEVICE_CTRL_BLK_GETGEOME)
    {
        struct rt_device_blk_geometry *geometry = (struct rt_device_blk_geometry *)args;

        geometry->sector_count = UTBLK_SECTORS;
        geometry->bytes_per_sector = UTBLK_BPS;
        geometry->block_size = UTBLK_BPS;
        return RT_EOK;
    }
    return -RT_EINVAL;
}

static rt_err_t utrtc_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev;
    if (cmd == RT_DEVICE_CTRL_RTC_GET_TIME)
    {
        *(time_t *)args = utrtc_time;
        return RT_EOK;
    }
    if (cmd == RT_DEVICE_CTRL_RTC_SET_TIME)
    {
        utrtc_time = *(time_t *)args;
        return RT_EOK;
    }
    return -RT_EINVAL;
}

static struct rt_device utst0_dev, utblk_dev, utrtc_dev;

static const struct rt_device_ops utst0_ops =
    { utst_init, utst_open, utst_close, utst0_read, utst0_write, utst0_control };
static const struct rt_device_ops utblk_ops =
    { utst_init, utst_open, utst_close, utblk_read, utblk_write, utblk_control };
static const struct rt_device_ops utrtc_ops =
    { utst_init, utst_open, utst_close, RT_NULL, RT_NULL, utrtc_control };

static void register_test_devices(void)
{
    if (devices_registered)
    {
        utst0_len = 0;
        return;
    }
    rt_memset(&utst0_dev, 0, sizeof(utst0_dev));
    utst0_dev.type = RT_Device_Class_Char;
    utst0_dev.ops = &utst0_ops;
    rt_device_register(&utst0_dev, "utst0", RT_DEVICE_FLAG_RDWR);

    rt_memset(&utblk_dev, 0, sizeof(utblk_dev));
    utblk_dev.type = RT_Device_Class_Block;
    utblk_dev.ops = &utblk_ops;
    rt_device_register(&utblk_dev, "utblk0", RT_DEVICE_FLAG_RDWR);

    rt_memset(&utrtc_dev, 0, sizeof(utrtc_dev));
    utrtc_dev.type = RT_Device_Class_RTC;
    utrtc_dev.ops = &utrtc_ops;
    rt_device_register(&utrtc_dev, "utrtc0", RT_DEVICE_FLAG_RDWR);
    devices_registered = RT_TRUE;
}

static AgentToolNode_t find_tool(const char *name)
{
    cJSON *call = cJSON_CreateObject();
    cJSON *function = cJSON_AddObjectToObject(call, "function");
    AgentToolNode_t node;

    cJSON_AddStringToObject(function, "name", name);
    node = search_agent_tool_node(call);
    cJSON_Delete(call);
    return node;
}

static const char *invoke(AgentToolNode_t node, const char *arguments)
{
    cJSON *args = arguments ? cJSON_Parse(arguments) : RT_NULL;

    node->execute_func(args, node);
    cJSON_Delete(args);
    if (!node->ret.messages || node->ret.messages->current_size != 1) return RT_NULL;
    return messages_get_content_idx(node->ret.messages, 0);
}

static void assert_contains(const char *text, const char *sub, const char *label)
{
    utest_assert(text != RT_NULL && sub != RT_NULL && rt_strstr(text, sub) != RT_NULL,
                 __FILE__, __LINE__, __func__, label);
}

static void assert_not_contains(const char *text, const char *sub, const char *label)
{
    utest_assert(text != RT_NULL && sub != RT_NULL && rt_strstr(text, sub) == RT_NULL,
                 __FILE__, __LINE__, __func__, label);
}

static void unit_tool_registration(void)
{
    int modules = agent_tool_module_count();
    int declared = 0;
    int missing = 0;
    int m, t;

    /*
     * Every tool module registers itself with AGENT_TOOL_MODULE_EXPORT, so the
     * set of tools depends on the Kconfig switches of this build. Walk the
     * module table instead of a fixed function list and check that every tool a
     * module declares actually reaches the agent tool list.
     */
    uassert_true(modules > 0);
    for (m = 0; m < modules; m++)
    {
        const char *module = agent_tool_module_name(m);

        uassert_not_null(module);
        uassert_true(agent_tool_module_tools(m) > 0);
        for (t = 0; t < agent_tool_module_tools(m); t++)
        {
            const char *name = agent_tool_module_tool_name(m, t);
            AgentToolNode_t node = name ? find_tool(name) : RT_NULL;

            declared++;
            if (!name || !node || !node->execute_func) missing++;
        }
    }
    /* the 7 general device tools alone guarantee this floor */
    uassert_true(declared >= 7);
    uassert_int_equal(missing, 0);
    /* out-of-range queries must be rejected instead of indexing the module table */
    uassert_ptr_equal(agent_tool_module_name(modules), RT_NULL);
    uassert_int_equal(agent_tool_module_tools(-1), 0);
}

static void unit_list_and_info(void)
{
    AgentToolNode_t node;
    const char *text;

    node = find_tool("device_list");
    uassert_not_null(node);
    if (!node) return;
    text = invoke(node, "{}");
    assert_contains(text, "name=uart0", "list shows real console uart0");
    assert_contains(text, "name=utst0 class=char", "list shows virtual char device");
    assert_contains(text, "name=utblk0 class=block", "list shows virtual block device");
    assert_contains(text, "name=utrtc0 class=rtc", "list shows virtual rtc device");

    text = invoke(node, "{\"class\":\"rtc\"}");
    assert_contains(text, "name=utrtc0", "class filter keeps matching device");
    assert_not_contains(text, "name=utst0 class", "class filter drops other classes");

    text = invoke(node, "{\"class\":\"bogus\"}");
    uassert_int_equal(node->ret.ret, -RT_EINVAL);
    assert_contains(text, "unknown class", "unknown class rejected");

    node = find_tool("device_info");
    uassert_not_null(node);
    if (!node) return;
    text = invoke(node, "{\"name\":\"utst0\"}");
    assert_contains(text, "class=char(0)", "info reports class id");
    assert_contains(text, "console=no", "info marks non-console device");
    assert_contains(text, "ref_count=0", "info reports closed reference count");
    assert_contains(text, "init,open,close,read,write,control", "info reports driver operations");
    text = invoke(node, "{\"name\":\"uart0\"}");
    assert_contains(text, "console=yes", "info marks console device");
    text = invoke(node, "{\"name\":\"nosuchdev\"}");
    assert_contains(text, "not found", "info rejects unknown device");
}

static void unit_lifecycle_refs(void)
{
    AgentToolNode_t open = find_tool("device_open");
    AgentToolNode_t close = find_tool("device_close");
    AgentToolNode_t read = find_tool("device_read");
    AgentToolNode_t write = find_tool("device_write");
    const char *text;

    uassert_not_null(open);
    uassert_not_null(close);
    uassert_not_null(read);
    uassert_not_null(write);
    if (!open || !close || !read || !write) return;

    text = invoke(read, "{\"name\":\"utst0\"}");
    assert_contains(text, "is not opened", "read before open rejected");
    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"x\"}");
    assert_contains(text, "is not opened", "write before open rejected");

    text = invoke(open, "{\"name\":\"utst0\",\"mode\":\"rdonly\"}");
    assert_contains(text, "open ok", "open read-only succeeds");
    assert_contains(text, "oflag=0x001", "read-only open flag passed through");
    assert_contains(text, "ref_count=1", "open increments reference count");
    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"hello\"}");
    assert_contains(text, "read-only", "write on read-only handle rejected");
    text = invoke(close, "{\"name\":\"utst0\"}");
    assert_contains(text, "ref_count=0", "close decrements reference count");
    text = invoke(close, "{\"name\":\"utst0\"}");
    assert_contains(text, "is not opened", "close before open rejected");

    text = invoke(open, "{\"name\":\"utst0\",\"mode\":\"rdwr\",\"flags\":[\"int_rx\"]}");
    assert_contains(text, "oflag=0x103", "open combines rdwr and int_rx flags");
    text = invoke(close, "{\"name\":\"utst0\"}");
    assert_contains(text, "close ok", "close after flagged open");
}

static void unit_char_io(void)
{
    AgentToolNode_t open = find_tool("device_open");
    AgentToolNode_t close = find_tool("device_close");
    AgentToolNode_t read = find_tool("device_read");
    AgentToolNode_t write = find_tool("device_write");
    const char *text;

    uassert_not_null(open);
    uassert_not_null(close);
    uassert_not_null(read);
    uassert_not_null(write);
    if (!open || !close || !read || !write) return;

    uassert_true(invoke(open, "{\"name\":\"utst0\",\"mode\":\"rdwr\"}") != RT_NULL);
    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"hello\"}");
    assert_contains(text, "written=5/5", "text write reports exact byte count");
    text = invoke(read, "{\"name\":\"utst0\",\"count\":10}");
    assert_contains(text, "count=5", "char read returns written bytes");
    assert_contains(text, "68 65 6c 6c 6f", "read hex preview matches text");
    assert_contains(text, "text: hello", "read text preview matches");
    text = invoke(read, "{\"name\":\"utst0\"}");
    assert_contains(text, "count=0", "char read consumes buffered data");

    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"de ad be ef\",\"encoding\":\"hex\"}");
    assert_contains(text, "written=4/4", "hex write decodes byte pairs");
    text = invoke(read, "{\"name\":\"utst0\",\"count\":4}");
    assert_contains(text, "de ad be ef", "hex written bytes read back");

    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"zz\",\"encoding\":\"hex\"}");
    assert_contains(text, "invalid hex", "invalid hex data rejected");
    text = invoke(write, "{\"name\":\"utst0\",\"data\":\"\"}");
    assert_contains(text, "'data' must be", "empty data rejected");

    text = invoke(close, "{\"name\":\"utst0\"}");
    assert_contains(text, "close ok", "close char device after io");
}

static void unit_block_io(void)
{
    AgentToolNode_t open = find_tool("device_open");
    AgentToolNode_t close = find_tool("device_close");
    AgentToolNode_t read = find_tool("device_read");
    AgentToolNode_t write = find_tool("device_write");
    AgentToolNode_t control = find_tool("device_control");
    const char *text;
    size_t i;

    uassert_not_null(open);
    uassert_not_null(close);
    uassert_not_null(read);
    uassert_not_null(write);
    uassert_not_null(control);
    if (!open || !close || !read || !write || !control) return;

    uassert_true(invoke(open, "{\"name\":\"utblk0\",\"mode\":\"rdwr\"}") != RT_NULL);
    text = invoke(control, "{\"name\":\"utblk0\",\"cmd\":\"BLK_GETGEOME\"}");
    assert_contains(text, "geometry: sector_count=16 bytes_per_sector=512 block_size=512",
                    "BLK_GETGEOME returns typed geometry");

    for (i = 0; i < 512; i++) args_buf[i] = 'A';
    args_buf[512] = '\0';
    {
        char req[640];

        rt_snprintf(req, sizeof(req), "{\"name\":\"utblk0\",\"data\":\"%s\",\"pos\":2}", args_buf);
        text = invoke(write, req);
    }
    assert_contains(text, "unit=blocks written=1/1 (512 bytes)", "block write counts whole blocks");
    text = invoke(read, "{\"name\":\"utblk0\",\"count\":1,\"pos\":2}");
    assert_contains(text, "unit=blocks count=1 (512 bytes)", "block read reports blocks and bytes");
    assert_contains(text, "text: AAAAAAAAAAAA", "block data round-trips");

    for (i = 0; i < 500; i++) args_buf[i] = 'x';
    args_buf[500] = '\0';
    {
        char req[600];

        rt_snprintf(req, sizeof(req), "{\"name\":\"utblk0\",\"data\":\"%s\"}", args_buf);
        text = invoke(write, req);
    }
    assert_contains(text, "whole blocks", "partial block write rejected");

    text = invoke(read, "{\"name\":\"utblk0\",\"count\":100,\"pos\":2}");
    assert_contains(text, "clamped", "oversized block read clamped");
    assert_contains(text, "count=4", "clamp limit is the 2048-byte transfer cap");

    text = invoke(close, "{\"name\":\"utblk0\"}");
    assert_contains(text, "close ok", "close block device after io");
}

static void unit_control_commands(void)
{
    AgentToolNode_t open = find_tool("device_open");
    AgentToolNode_t close = find_tool("device_close");
    AgentToolNode_t control = find_tool("device_control");
    const char *text;

    uassert_not_null(open);
    uassert_not_null(close);
    uassert_not_null(control);
    if (!open || !close || !control) return;

    uassert_true(invoke(open, "{\"name\":\"utst0\",\"mode\":\"rdwr\"}") != RT_NULL);
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"0x99\"}");
    assert_contains(text, "control ok", "raw numeric no-arg command succeeds");
    assert_contains(text, "no result payload", "raw command reports no payload");
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"0x98\"}");
    assert_contains(text, "control failed", "driver command failure surfaced");
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"BLK_GETGEOME\",\"arg\":1}");
    assert_contains(text, "takes no argument", "argument to no-arg command rejected");
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"RTC_GET_ALARM\"}");
    assert_contains(text, "not supported", "struct-argument command refused");
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"NOPE\"}");
    assert_contains(text, "unknown command", "unknown command name rejected");
    text = invoke(control, "{\"name\":\"utst0\",\"cmd\":\"RTC_SET_TIME\"}");
    assert_contains(text, "requires a numeric 'arg'", "typed command missing arg rejected");
    uassert_int_equal(control->ret.ret, -RT_EINVAL);
    text = invoke(close, "{\"name\":\"utst0\"}");
    assert_contains(text, "close ok", "close char device after control");

    uassert_true(invoke(open, "{\"name\":\"utrtc0\",\"mode\":\"rdwr\"}") != RT_NULL);
    text = invoke(control, "{\"name\":\"utrtc0\",\"cmd\":\"RTC_SET_TIME\",\"arg\":1700001234}");
    assert_contains(text, "seconds=1700001234", "RTC_SET_TIME passes typed value");
    text = invoke(control, "{\"name\":\"utrtc0\",\"cmd\":\"RTC_GET_TIME\"}");
    assert_contains(text, "time: seconds=1700001234", "RTC_GET_TIME returns typed value");
    text = invoke(close, "{\"name\":\"utrtc0\"}");
    assert_contains(text, "close ok", "close rtc device after control");
}

static void unit_console_guards(void)
{
    AgentToolNode_t read = find_tool("device_read");
    AgentToolNode_t close = find_tool("device_close");
    const char *text;

    uassert_not_null(read);
    uassert_not_null(close);
    if (!read || !close) return;

    text = invoke(read, "{\"name\":\"uart0\",\"count\":1}");
    assert_contains(text, "console", "console read refused");
    text = invoke(close, "{\"name\":\"uart0\"}");
    assert_contains(text, "console", "console close refused");
}

static void unit_error_paths(void)
{
    AgentToolNode_t open = find_tool("device_open");
    AgentToolNode_t close = find_tool("device_close");
    AgentToolNode_t info = find_tool("device_info");
    AgentToolNode_t control = find_tool("device_control");
    const char *text;

    uassert_not_null(open);
    uassert_not_null(close);
    uassert_not_null(info);
    uassert_not_null(control);
    if (!open || !close || !info || !control) return;

    text = invoke(info, "{\"name\":\"missingdev\"}");
    assert_contains(text, "not found", "unknown device reported with guidance");
    text = invoke(open, "{\"name\":\"\"}");
    assert_contains(text, "non-empty device name", "empty device name rejected");
    text = invoke(open, "{\"name\":\"utst0\",\"flags\":[\"bogus\"]}");
    assert_contains(text, "unknown open flag", "unknown open flag rejected");
    uassert_true(invoke(open, "{\"name\":\"utst0\",\"mode\":\"rdwr\"}") != RT_NULL);
    text = invoke(control, "{\"name\":\"utst0\"}");
    assert_contains(text, "'cmd' must be", "missing control command rejected");
    uassert_true(invoke(close, "{\"name\":\"utst0\"}") != RT_NULL);
}

static void unit_skill_catalog(void)
{
    const struct agent_skill *skill = agent_skill_find("device");
    const struct agent_skill *commands = agent_skill_find("device-commands");
    AgentToolNode_t node;
    const char *text;

    uassert_not_null(skill);
    uassert_not_null(commands);
    if (skill)
        assert_contains(skill->content, "device_open", "device skill carries framework workflow");
    if (commands)
        assert_contains(commands->content, "BLK_GETGEOME", "command skill carries real commands");

    node = find_tool("skill");
    uassert_not_null(node);
    if (!node || !commands) return;
    {
        cJSON *args = cJSON_Parse("{\"name\":\"device-commands\"}");

        node->execute_func(args, node);
        cJSON_Delete(args);
    }
    text = node->ret.messages ? messages_get_content_idx(node->ret.messages, 0) : RT_NULL;
    uassert_true(node->ret.ret == RT_EOK && text != RT_NULL);
    if (text) uassert_str_equal(text, commands->content);
}

static rt_err_t utest_tc_init(void)
{
    init_tools();
    register_test_devices();
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    rt_device_t dev;

    dev = rt_device_find("utst0");
    if (dev && dev->ref_count) rt_device_close(dev);
    dev = rt_device_find("utblk0");
    if (dev && dev->ref_count) rt_device_close(dev);
    dev = rt_device_find("utrtc0");
    if (dev && dev->ref_count) rt_device_close(dev);
    return RT_EOK;
}

static void testcase(void)
{
    UTEST_UNIT_RUN(unit_tool_registration);
    UTEST_UNIT_RUN(unit_list_and_info);
    UTEST_UNIT_RUN(unit_lifecycle_refs);
    UTEST_UNIT_RUN(unit_char_io);
    UTEST_UNIT_RUN(unit_block_io);
    UTEST_UNIT_RUN(unit_control_commands);
    UTEST_UNIT_RUN(unit_console_guards);
    UTEST_UNIT_RUN(unit_error_paths);
    UTEST_UNIT_RUN(unit_skill_catalog);
}
UTEST_TC_EXPORT(testcase, "agent.device_tools", utest_tc_init, utest_tc_cleanup, 60);
