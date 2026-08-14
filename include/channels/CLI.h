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
#define CLI_THREAD_STACK_SIZE  1024   // 修复：重命名，调大栈防止溢出

enum CLI_channelInputStat
{
    CLI_WAIT_NORMAL,
    CLI_WAIT_SPEC_KEY,
    CLI_WAIT_FUNC_KEY,
};

typedef struct CLI_channel
{
    /* thread */
    struct rt_thread thread;
    rt_uint8_t thread_stack[CLI_THREAD_STACK_SIZE];

    enum CLI_channelInputStat stat;
    /* sem */
    struct rt_semaphore rx_sem;
    rt_bool_t sem_inited;    // 标记信号量是否初始化，防止重复init

    char CLI_history[CLI_HISTORY_LINES][CLI_CMD_BUFFER_SIZE];
    rt_uint16_t history_count;
    rt_uint16_t history_current;

    char line[CLI_CMD_BUFFER_SIZE];
    rt_uint16_t line_position;
    rt_uint16_t line_curpos;  // 修复：uint8→uint16，防止超过255溢出

    rt_device_t device;
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size);

    MessageHub_t message_hub;
    rt_bool_t thread_running; // 线程运行标志
} CLI_channel, *CLI_channel_t;

int AgentCLIChannel(MessageHub_t message_hub, Context_t context);
#endif /* __AGENT_CLI_H__ */