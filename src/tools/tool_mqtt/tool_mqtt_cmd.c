/*
 * MQTT 工具 - MSH 调试命令（mqtt_tool ...）
 * 命令行入口：启动/停止、订阅、发布、规则、收取、模拟报文。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#include <stdlib.h>

/* ==========================================================================
 * MSH 测试命令：mqtt_tool <start|stop|status|list|sub|unsub|pub|recv|sim|flush>
 * ========================================================================== */

/* 单次 rt_kprintf 的输出上限（RT_CONSOLEBUF_SIZE，默认 256）以内分段打印 */
#define MQTT_TOOL_PRINT_CHUNK   96

/*
 * @brief 分段打印长文本
 * @note  rt_kprintf 内部使用共享缓冲 rt_log_buf[RT_CONSOLEBUF_SIZE]，
 *        超过该长度会被静默截断；历史/状态等长输出必须按行分段打印
 * @param text 待打印文本
 */
static void mqtt_tool_print(const char *text)
{
    char chunk[MQTT_TOOL_PRINT_CHUNK + 1];
    const char *p = text;
    rt_bool_t ended_with_nl = RT_FALSE;

    if (text == RT_NULL)
    {
        return;
    }

    while (*p != '\0')
    {
        rt_size_t len = 0;

        while (p[len] != '\0' && p[len] != '\n' && len < MQTT_TOOL_PRINT_CHUNK)
        {
            len++;
        }
        rt_memcpy(chunk, p, len);
        chunk[len] = '\0';
        rt_kprintf("%s", chunk);
        p += len;

        if (*p == '\n')
        {
            rt_kprintf("\n");
            p++;
            ended_with_nl = RT_TRUE;
        }
        else
        {
            ended_with_nl = RT_FALSE;
        }
    }

    if (!ended_with_nl)
    {
        rt_kprintf("\n");
    }
}

static void mqtt_tool_usage(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  mqtt_tool connect                        - connect (start + wait for connection)\n");
    rt_kprintf("  mqtt_tool disconnect                     - disconnect (subscriptions are kept)\n");
    rt_kprintf("  mqtt_tool start | stop                   - alias of connect | disconnect\n");
    rt_kprintf("  mqtt_tool status                         - show client, subscriptions and rules\n");
    rt_kprintf("  mqtt_tool sub <topic> [filter|auto|poll] - subscribe (default filter)\n");
    rt_kprintf("  mqtt_tool unsub [topic]                  - unsubscribe topic (empty = all)\n");
    rt_kprintf("  mqtt_tool pub [topic] <message>          - publish message\n");
    rt_kprintf("  mqtt_tool rule <topic> [field] <op> <n>  - set threshold rule (op: > >= < <= == !=)\n");
    rt_kprintf("  mqtt_tool rule del [topic]               - remove rule(s)\n");
    rt_kprintf("  mqtt_tool rules                          - list subscriptions and rules\n");
    rt_kprintf("  mqtt_tool history [topic] [max]          - recent messages + numeric summary (read-only)\n");
    rt_kprintf("  mqtt_tool recv [max] [topic]             - fetch buffered messages (consume)\n");
    rt_kprintf("  mqtt_tool sim <topic> <payload>          - simulate an inbound message\n");
    rt_kprintf("  mqtt_tool flush                          - drop buffered messages\n");
}

/*
 * @brief 模拟收到一条订阅消息（无需 broker，用于验证「订阅 -> agent 分析」链路）
 * @param topic   话题
 * @param payload 内容
 */
static void mqtt_tool_simulate(const char *topic, const char *payload)
{
    /* 静态缓冲：仅在 MSH 线程使用，避免在 4KB 的 shell 栈上放大对象 */
    static mqtt_rx_item_t item;
    rt_size_t copy_len;

    if (topic == RT_NULL || payload == RT_NULL)
    {
        return;
    }
    if (s_rx_queue == RT_NULL)
    {
        rt_kprintf("mqtt rx queue not ready, run 'mqtt_tool start' first\n");
        return;
    }

    rt_memset(&item, 0, sizeof(item));
    rt_strncpy(item.topic, topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
    copy_len = rt_strlen(payload);
    if (copy_len > (MQTT_TOOL_PAYLOAD_MAX_LEN - 1))
    {
        copy_len = MQTT_TOOL_PAYLOAD_MAX_LEN - 1;
        item.truncated = 1;
    }
    rt_memcpy(item.payload, payload, copy_len);
    item.payload[copy_len] = '\0';
    item.payload_len = (rt_uint16_t)copy_len;
    item.raw_len = item.payload_len;

    if (rt_mq_send(s_rx_queue, &item, sizeof(item)) != RT_EOK)
    {
        rt_kprintf("mqtt rx queue full\n");
        return;
    }
    rt_kprintf("simulated message on '%s' enqueued\n", topic);
}

/*
 * @brief MSH 命令入口
 * @note  结果缓冲一律堆分配：MSH 线程栈仅 FINSH_THREAD_STACK_SIZE（默认 4KB），
 *        在命令处理函数里放几百字节局部数组会栈溢出并破坏堆内存
 * @param argc 参数个数
 * @param argv 参数列表
 * @return 0 成功，-1 失败
 */
static int mqtt_tool(int argc, char **argv)
{
    char *result = RT_NULL;
    int ret = 0;

    if (argc < 2)
    {
        mqtt_tool_usage();
        return -1;
    }

    /* status/sub/unsub/pub 用中等缓冲；recv 用大缓冲；history 用超大缓冲 */
    {
        rt_size_t need = MQTT_TOOL_RESULT_MID;
        if (rt_strcmp(argv[1], "recv") == 0)
        {
            need = MQTT_TOOL_RESULT_LARGE;
        }
        else if (rt_strcmp(argv[1], "history") == 0)
        {
            need = MQTT_TOOL_RESULT_XLARGE;
        }
        result = rt_malloc(need);
    }
    if (result == RT_NULL)
    {
        rt_kprintf("out of memory\n");
        return -1;
    }
    result[0] = '\0';

    if (rt_strcmp(argv[1], "connect") == 0 || rt_strcmp(argv[1], "start") == 0)
    {
        rt_int32_t timeout_ms = (argc >= 3) ? (rt_int32_t)atoi(argv[2]) : 0;
        ret = (mqtt_do_connect(timeout_ms, result, MQTT_TOOL_RESULT_MID) == RT_EOK) ? 0 : -1;
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "disconnect") == 0 || rt_strcmp(argv[1], "stop") == 0)
    {
        mqtt_do_disconnect(result, MQTT_TOOL_RESULT_MID);
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "status") == 0 || rt_strcmp(argv[1], "list") == 0)
    {
        mqtt_do_list(result, MQTT_TOOL_RESULT_MID);
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "sub") == 0 && argc >= 3)
    {
        mqtt_deliver_mode_t mode = MQTT_DELIVER_FILTER;
        if (argc >= 4)
        {
            if (rt_strcmp(argv[3], "auto") != 0 && rt_strcmp(argv[3], "poll") != 0 &&
                rt_strcmp(argv[3], "filter") != 0)
            {
                rt_kprintf("mode must be filter|auto|poll\n");
                rt_free(result);
                return -1;
            }
            mode = mqtt_mode_from_string(argv[3]);
        }
        if (mqtt_do_subscribe(argv[2], mode, result, MQTT_TOOL_RESULT_MID) != RT_EOK)
        {
            ret = -1;
        }
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "rule") == 0)
    {
        /* rule <topic> [field] <op> <value> | rule del <topic> | rules */
        if (argc >= 3 && (rt_strcmp(argv[2], "del") == 0 || rt_strcmp(argv[2], "clear") == 0))
        {
            const char *rtopic = (argc >= 4) ? argv[3] : "*";
            if (mqtt_do_rule("clear", rtopic, RT_NULL, ">", 0.0, result, MQTT_TOOL_RESULT_MID) != RT_EOK)
            {
                ret = -1;
            }
            mqtt_tool_print(result);
        }
        else if (argc >= 5)
        {
            const char *rtopic = argv[2];
            const char *rfield = RT_NULL;
            const char *rop;
            double rvalue;
            /* 4 个参数=整条 payload 比较；5 个参数=指定 JSON 字段 */
            if (argc >= 6)
            {
                rfield = argv[3];
                rop = argv[4];
                rvalue = atof(argv[5]);
            }
            else
            {
                rop = argv[3];
                rvalue = atof(argv[4]);
            }
            if (mqtt_do_rule("set", rtopic, rfield, rop, rvalue, result, MQTT_TOOL_RESULT_MID) != RT_EOK)
            {
                ret = -1;
            }
            mqtt_tool_print(result);
        }
        else
        {
            mqtt_do_list(result, MQTT_TOOL_RESULT_MID);
            mqtt_tool_print(result);
        }
    }
    else if (rt_strcmp(argv[1], "rules") == 0)
    {
        mqtt_do_list(result, MQTT_TOOL_RESULT_MID);
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "unsub") == 0)
    {
        if (mqtt_do_unsubscribe((argc >= 3) ? argv[2] : RT_NULL, result, MQTT_TOOL_RESULT_MID) != RT_EOK)
        {
            ret = -1;
        }
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "pub") == 0 && argc >= 3)
    {
        /* pub <message> 使用默认话题；pub <topic> <message> 指定话题 */
        if (argc >= 4)
        {
            ret = (mqtt_do_publish(argv[2], argv[3], result, MQTT_TOOL_RESULT_MID) == RT_EOK) ? 0 : -1;
        }
        else
        {
            ret = (mqtt_do_publish(RT_NULL, argv[2], result, MQTT_TOOL_RESULT_MID) == RT_EOK) ? 0 : -1;
        }
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "recv") == 0)
    {
        int max = (argc >= 3) ? atoi(argv[2]) : MQTT_TOOL_POLL_DEPTH;
        const char *filter = (argc >= 4) ? argv[3] : RT_NULL;

        mqtt_do_receive(max, filter, result, MQTT_TOOL_RESULT_LARGE);
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "history") == 0)
    {
        const char *filter = (argc >= 3) ? argv[2] : RT_NULL;
        int max = (argc >= 4) ? atoi(argv[3]) : MQTT_TOOL_HISTORY_DEPTH;

        mqtt_do_history(filter, max, result, MQTT_TOOL_RESULT_XLARGE);
        mqtt_tool_print(result);
    }
    else if (rt_strcmp(argv[1], "sim") == 0 && argc >= 4)
    {
        mqtt_tool_simulate(argv[2], argv[3]);
    }
    else if (rt_strcmp(argv[1], "flush") == 0)
    {
        int flushed = poll_clear();
        if (flushed < 0)
        {
            rt_kprintf("mqtt state busy, retry later\n");
            ret = -1;
        }
        else
        {
            rt_kprintf("flushed %d buffered message(s)\n", flushed);
        }
    }
    else
    {
        mqtt_tool_usage();
        ret = -1;
    }

    rt_free(result);
    return ret;
}

MSH_CMD_EXPORT(mqtt_tool, mqtt tool test: start|stop|status|sub|unsub|pub|recv|sim|flush);
