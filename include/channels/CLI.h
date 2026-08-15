#ifndef __AGENT_CLI_H__
#define __AGENT_CLI_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"
#include "shell.h"

#define LOG_TAG "Agent.Channels.CLI"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define CLI_CMD_BUFFER_SIZE    512
#define CLI_HISTORY_LINES          5
#define CLI_THREAD_STACK_SIZE  1024

enum CLI_channelInputStat
{
    CLI_WAIT_NORMAL,
    CLI_WAIT_SPEC_KEY,
    CLI_WAIT_FUNC_KEY,
};

/* CLI 通道结构体 */
typedef struct CLI_channel
{
    struct rt_thread thread;
    rt_uint8_t thread_stack[CLI_THREAD_STACK_SIZE];

    enum CLI_channelInputStat stat;
    struct rt_semaphore rx_sem;
    rt_bool_t sem_inited;

    char CLI_history[CLI_HISTORY_LINES][CLI_CMD_BUFFER_SIZE];
    rt_uint16_t history_count;
    rt_uint16_t history_current;

    char line[CLI_CMD_BUFFER_SIZE];
    rt_uint16_t line_position;
    rt_uint16_t line_curpos;

    rt_device_t device;
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size);

    MessageHub_t message_hub;
    rt_bool_t thread_running;
} CLI_channel, *CLI_channel_t;

/*
 * @brief CLI 通道初始化（创建 readline 线程）
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return RT_EOK 成功
 */
int agent_cli_channel(MessageHub_t message_hub, Context_t context);

#endif /* __AGENT_CLI_H__ */