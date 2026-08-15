#include "CLI.h"

#define LOG_TAG "Agent.Channels.CLI"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

static CLI_channel_t g_handle = RT_NULL;

/* CLI 通道 ops 实例（init=agent_cli_channel, reset=agent_cli_stop） */
AgentChannelOps agent_cli_ops = { agent_cli_channel, agent_cli_stop };

/*
 * @brief 阻塞获取一个字符
 * @return 读取到的字符
 */
static rt_uint8_t CLI_getc(CLI_channel_t handle)
{
    rt_uint8_t ch = 0;
    while (rt_device_read(handle->device, -1, &ch, 1) != 1)
    {
        if (!handle->thread_running)
            return 0x04;
        rt_sem_take(handle->rx_sem, RT_WAITING_FOREVER);
        if (!handle->thread_running)
            return 0x04;
    }
    return ch;
}

/*
 * @brief 设备接收回调
 */
static rt_err_t CLI_rxcb(rt_device_t dev, rt_size_t size)
{
    /* 使用全局指针而非 dev->user_data，避免覆盖串口驱动私有数据 */
    CLI_channel_t handle = g_handle;
    if (handle && handle->rx_sem)
    {
        rt_sem_release(handle->rx_sem);
    }
    return RT_EOK;
}

/*
 * @brief 刷新命令行回显（ANSI 清除当前行）
 */
static rt_bool_t CLI_handle_history(CLI_channel_t handle, const char *prompt)
{
    rt_kprintf("\033[2K\r%s%s", prompt, handle->line);
    return RT_FALSE;
}

/*
 * @brief 提交当前行到历史记录
 */
static void CLI_push_history(CLI_channel_t handle)
{
    if (handle->line_position == 0)
        return;

    if (handle->history_count > 0)
    {
        if (rt_strncmp(handle->CLI_history[handle->history_count - 1], handle->line, CLI_CMD_BUFFER_SIZE) == 0)
        {
            handle->history_current = handle->history_count;
            return;
        }
    }

    if (handle->history_count >= CLI_HISTORY_LINES)
    {
        for (int index = 0; index < CLI_HISTORY_LINES - 1; index++)
        {
            rt_memcpy(handle->CLI_history[index],
                      handle->CLI_history[index + 1],
                      CLI_CMD_BUFFER_SIZE);
        }
        rt_memset(handle->CLI_history[CLI_HISTORY_LINES - 1], 0, CLI_CMD_BUFFER_SIZE);
        rt_memcpy(handle->CLI_history[CLI_HISTORY_LINES - 1], handle->line, handle->line_position);
        handle->history_count = CLI_HISTORY_LINES;
    }
    else
    {
        rt_memset(handle->CLI_history[handle->history_count], 0, CLI_CMD_BUFFER_SIZE);
        rt_memcpy(handle->CLI_history[handle->history_count], handle->line, handle->line_position);
        handle->history_count++;
    }
    handle->history_current = handle->history_count;
}

/*
 * @brief readline 实现（支持光标移动、历史、退格）
 * @param handle     CLI 通道句柄
 * @param prompt      提示符
 * @param buffer      输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 输入行长度
 */
static int CLI_readline(CLI_channel_t handle, const char *prompt, char *buffer, int buffer_size)
{
    rt_uint8_t ch;

start:
    rt_kprintf(prompt);

    while (1)
    {
        ch = CLI_getc(handle);

        if (ch == 0x1b)
        {
            handle->stat = CLI_WAIT_SPEC_KEY;
            continue;
        }
        else if (handle->stat == CLI_WAIT_SPEC_KEY)
        {
            if (ch == 0x5b)
            {
                handle->stat = CLI_WAIT_FUNC_KEY;
                continue;
            }
            handle->stat = CLI_WAIT_NORMAL;
        }
        else if (handle->stat == CLI_WAIT_FUNC_KEY)
        {
            handle->stat = CLI_WAIT_NORMAL;
            switch (ch)
            {
                case 0x41:
                    if (handle->history_current > 0)
                    {
                        handle->history_current--;
                        rt_strncpy(handle->line, handle->CLI_history[handle->history_current], CLI_CMD_BUFFER_SIZE);
                        handle->line_position = rt_strlen(handle->line);
                        handle->line_curpos = handle->line_position;
                        CLI_handle_history(handle, prompt);
                    }
                    continue;

                case 0x42:
                    if (handle->history_current < handle->history_count - 1)
                    {
                        handle->history_current++;
                        rt_strncpy(handle->line, handle->CLI_history[handle->history_current], CLI_CMD_BUFFER_SIZE);
                        handle->line_position = rt_strlen(handle->line);
                        handle->line_curpos = handle->line_position;
                        CLI_handle_history(handle, prompt);
                    }
                    else
                    {
                        handle->history_current = handle->history_count;
                        rt_memset(handle->line, 0, CLI_CMD_BUFFER_SIZE);
                        handle->line_position = 0;
                        handle->line_curpos = 0;
                        CLI_handle_history(handle, prompt);
                    }
                    continue;

                case 0x44:
                    if (handle->line_curpos > 0)
                    {
                        rt_kprintf("\033[D");
                        handle->line_curpos--;
                    }
                    continue;

                case 0x43:
                    if (handle->line_curpos < handle->line_position)
                    {
                        rt_kprintf("\033[C");
                        handle->line_curpos++;
                    }
                    continue;
                default:
                    break;
            }
        }

        if (ch == '\0' || ch == 0xFF)
            continue;

        if (ch == 0x7f || ch == 0x08)
        {
            if (handle->line_curpos == 0)
                continue;

            handle->line_curpos--;
            handle->line_position--;

            rt_memmove(&handle->line[handle->line_curpos],
                       &handle->line[handle->line_curpos + 1],
                       handle->line_position - handle->line_curpos);
            handle->line[handle->line_position] = '\0';

            CLI_handle_history(handle, prompt);
            continue;
        }

        if (ch == '\r' || ch == '\n')
        {
            CLI_push_history(handle);
            rt_kprintf("\n");
            if (handle->line_position == 0)
            {
                goto start;
            }
            rt_strncpy(buffer, handle->line, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            int len = handle->line_position;

            rt_memset(handle->line, 0, CLI_CMD_BUFFER_SIZE);
            handle->line_curpos = handle->line_position = 0;
            return len;
        }

        if (ch == 0x04)
        {
            return 0;
        }

        if (ch == '\t')
        {
            continue;
        }

        if (handle->line_position >= (CLI_CMD_BUFFER_SIZE - 1))
        {
            continue;
        }

        if (handle->line_curpos < handle->line_position)
        {
            rt_memmove(&handle->line[handle->line_curpos + 1],
                       &handle->line[handle->line_curpos],
                       handle->line_position - handle->line_curpos);
        }
        handle->line[handle->line_curpos] = ch;
        handle->line_curpos++;
        handle->line_position++;
        handle->line[handle->line_position] = '\0';

        CLI_handle_history(handle, prompt);
    }
}

/*
 * @brief CLI 工作线程：读取用户输入 -> 发送到 message hub -> 获取结果
 * @param p 线程参数（CLI_channel_t 句柄）
 */
static void CLI_run(void *p)
{
    CLI_channel_t handle = (CLI_channel_t)p;
    char input_buffer[CLI_CMD_BUFFER_SIZE] = {0};
    const char *device_name = RT_CONSOLE_DEVICE_NAME;
    Messages_t input_messages = RT_NULL;
    Messages_t output_message = RT_NULL;

    handle->thread_running = RT_TRUE;
    handle->device = rt_device_find(device_name);
    if (handle->device == RT_NULL)
    {
        LOG_D("The msh device find failed.");
        handle->thread_running = RT_FALSE;
        handle->thread = RT_NULL;
        /* 通知 agent_cli_stop 线程已完全退出，避免其等待 exit_sem 卡死 */
        rt_sem_release(handle->exit_sem);
        return;
    }

    handle->rx_indicate = handle->device->rx_indicate;
    rt_device_set_rx_indicate(handle->device, CLI_rxcb);

    rt_kprintf("\nPress CTRL+D to exit CLI shell.\n");

    while (handle->thread_running)
    {
        int length = CLI_readline(handle, "Enter command: ", input_buffer, sizeof(input_buffer));

        if (length == 0)
        {
            rt_kprintf("Exit terminal.\n");
            break;
        }
        else if (length > 0)
        {
            input_messages = messages_create(0);
            if (input_messages == RT_NULL)
            {
                LOG_E("messages_create fail");
                continue;
            }
            if (messages_append(input_messages, TYPE_TEXT, input_buffer) != RT_EOK)
            {
                LOG_E("messages_append fail");
                messages_destroy(input_messages);
                continue;
            }

            /* 所有权移交：put 成功后由 main_loop 消费并释放，此处不得销毁 */
            if (handle->message_hub->put_message(handle->message_hub,
                                                 input_messages,
                                                 handle->message_hub->input_mailbox) != RT_EOK)
            {
                LOG_E("put input message fail");
                messages_destroy(input_messages);
                continue;
            }

            /* 可中断的输出等待：agent 停止时能及时退出，避免清理卡死 */
            output_message = RT_NULL;
            while (handle->thread_running)
            {
                output_message = message_hub_get_timeout(handle->message_hub,
                                                         handle->message_hub->output_mailbox,
                                                         rt_tick_from_millisecond(100));
                if (output_message != RT_NULL)
                {
                    break;
                }
            }

            rt_kprintf("\n");
            if (output_message)
            {
                messages_destroy(output_message);
            }
        }

        rt_memset(input_buffer, 0, sizeof(input_buffer));
    }

    rt_device_set_rx_indicate(handle->device, handle->rx_indicate);
    handle->device = RT_NULL;
    handle->thread_running = RT_FALSE;
    /* 动态线程退出后空闲线程自动清理，清零避免再次使用时野指针 */
    handle->thread = RT_NULL;
    rt_kprintf(FINSH_PROMPT);

    /* 通知 agent_cli_stop 线程已完全退出 */
    rt_sem_release(handle->exit_sem);
}

/*
 * @brief 停止 CLI 通道线程并等待退出，随后释放句柄与信号量（幂等）
 * @note 作为 AgentChannelOps.reset 实现，用于清理流程，
 *       确保线程不再访问 message_hub / context
 */
void agent_cli_stop(void)
{
    CLI_channel_t handle = g_handle;
    if (handle == RT_NULL)
    {
        return;
    }

    /* 线程运行中：请求停止并唤醒可能阻塞在 CLI_getc() 中 rx_sem 上的线程 */
    if (handle->thread_running == RT_TRUE)
    {
        handle->thread_running = RT_FALSE;
        rt_sem_release(handle->rx_sem);
    }

    /* 等待线程实际退出（CLI_run 最后一步才 release exit_sem；
       线程已退出（如用户 CTRL+D 自行退出）时立即返回） */
    rt_sem_take(handle->exit_sem, RT_WAITING_FOREVER);

    /* 线程已完全退出，安全释放句柄与信号量（先置空全局，防 rx 回调竞态） */
    g_handle = RT_NULL;
    if (handle->rx_sem)
    {
        rt_sem_delete(handle->rx_sem);
        handle->rx_sem = RT_NULL;
    }
    if (handle->exit_sem)
    {
        rt_sem_delete(handle->exit_sem);
        handle->exit_sem = RT_NULL;
    }
    rt_free(handle);

    LOG_I("CLI channel stopped");
}

/*
 * @brief CLI 通道初始化
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return RT_EOK 成功
 */
int agent_cli_channel(MessageHub_t message_hub, Context_t context)
{
    CLI_channel_t handle = g_handle;

    if (message_hub == RT_NULL || context == RT_NULL)
    {
        LOG_E("CLI channel requires valid message_hub and context");
        return -RT_EINVAL;
    }

    /* 如果已有运行中的实例，先停止 */
    if (handle != RT_NULL && handle->thread_running == RT_TRUE)
    {
        LOG_W("CLI channel already running!");
        return RT_EOK;
    }

    /* 清理旧实例（兜底：正常情况下 agent_cli_stop 已释放句柄） */
    if (handle != RT_NULL)
    {
        agent_cli_stop();
    }

    handle = (CLI_channel_t)rt_malloc(sizeof(CLI_channel));
    if (handle == RT_NULL)
    {
        LOG_E("CLI channel malloc failed");
        return -RT_ENOMEM;
    }
    rt_memset(handle, 0, sizeof(CLI_channel));

    handle->message_hub = message_hub;
    handle->stat = CLI_WAIT_NORMAL;

    handle->rx_sem = rt_sem_create("CLI_rxsem", 0, RT_IPC_FLAG_FIFO);
    if (handle->rx_sem == RT_NULL)
    {
        rt_free(handle);
        LOG_E("rx sem create failed");
        return -RT_ENOMEM;
    }

    handle->exit_sem = rt_sem_create("CLI_exit", 0, RT_IPC_FLAG_FIFO);
    if (handle->exit_sem == RT_NULL)
    {
        rt_sem_delete(handle->rx_sem);
        rt_free(handle);
        LOG_E("exit sem create failed");
        return -RT_ENOMEM;
    }

    #if defined(RT_VERSION_CHECK) && (RTTHREAD_VERSION >= RT_VERSION_CHECK(5, 1, 0))
    rt_uint8_t prio = RT_SCHED_PRIV(rt_thread_self()).current_priority + 1;
#else
    rt_uint8_t prio = rt_thread_self()->current_priority + 1;
#endif
    handle->thread = rt_thread_create("CLI_channel", CLI_run, handle,
                                      CLI_THREAD_STACK_SIZE, prio, 10);
    if (handle->thread == RT_NULL)
    {
        rt_sem_delete(handle->rx_sem);
        rt_sem_delete(handle->exit_sem);
        rt_free(handle);
        LOG_E("The CLI channel thread create failed.");
        return -RT_ENOMEM;
    }

    g_handle = handle;
    rt_thread_startup(handle->thread);
    return RT_EOK;
}