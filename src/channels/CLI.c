#include "CLI.h"

static CLI_channel_t g_handle = RT_NULL;

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
            messages_append(input_messages, TYPE_TEXT, input_buffer);

            if (input_messages == RT_NULL)
            {
                LOG_E("messages_create fail");
                continue;
            }

            handle->message_hub->put_message(handle->message_hub,
                                             input_messages,
                                             handle->message_hub->input_mailbox);

            output_message = handle->message_hub->get_message(handle->message_hub,
                                                              handle->message_hub->output_mailbox);

            rt_kprintf("\n");
            messages_destroy(input_messages);
            messages_destroy(output_message);
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
 * @brief 停止 CLI 通道线程并等待退出
 */
void agent_cli_stop(void)
{
    CLI_channel_t handle = g_handle;
    if (handle == RT_NULL || handle->thread_running != RT_TRUE)
        return;

    handle->thread_running = RT_FALSE;

    /* 唤醒可能阻塞在 CLI_getc() 中 rx_sem 上的线程 */
    rt_sem_release(handle->rx_sem);

    /* 等待线程实际退出（CLI_run 退出时会 release exit_sem） */
    rt_sem_take(handle->exit_sem, RT_WAITING_FOREVER);

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

    /* 如果已有运行中的实例，先停止 */
    if (handle != RT_NULL && handle->thread_running == RT_TRUE)
    {
        LOG_W("CLI channel already running!");
        return RT_EOK;
    }

    /* 清理旧实例 */
    if (handle != RT_NULL)
    {
        if (handle->rx_sem)
            rt_sem_delete(handle->rx_sem);
        if (handle->exit_sem)
            rt_sem_delete(handle->exit_sem);
        /* 动态线程退出后空闲线程自动清理，无需手动 rt_thread_delete */
        rt_free(handle);
        g_handle = RT_NULL;
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