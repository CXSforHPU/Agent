#ifndef __AGENT_CLI_H__
#define __AGENT_CLI_H__
#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"
#include "shell.h"
#include "AgentChannel.h"

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
    rt_thread_t thread;
    enum CLI_channelInputStat stat;
    rt_sem_t rx_sem;
    rt_sem_t exit_sem;
    /* 线程栈由 rt_thread_create 动态分配，不再需要 thread_stack 数组 */

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

/*
 * @brief 停止 CLI 通道线程并等待退出，随后释放句柄与信号量（幂等）
 * @note 作为 AgentChannelOps.reset 实现，用于清理流程，
 *       确保线程不再访问 message_hub / context
 */
void agent_cli_stop(void);

/* CLI 通道 ops 实例（init=agent_cli_channel, reset=agent_cli_stop） */
extern AgentChannelOps agent_cli_ops;

#endif /* __AGENT_CLI_H__ */