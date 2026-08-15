#include "CLI.h"

static CLI_channel handle = {0};

/*
 * @brief 阻塞获取一个字符
 * @return 读取到的字符
 */
static rt_uint8_t CLI_getc(void)
{
    rt_uint8_t ch = 0;
    while (rt_device_read(handle.device, -1, &ch, 1) != 1)
    {
        rt_sem_take(&(handle.rx_sem), RT_WAITING_FOREVER);
    }
    return ch;
}

/*
 * @brief 设备接收回调
 */
static rt_err_t CLI_rxcb(rt_device_t dev, rt_size_t size)
{
    rt_sem_release(&(handle.rx_sem));
    return RT_EOK;
}

/*
 * @brief 刷新命令行回显（ANSI 清除当前行）
 */
static rt_bool_t CLI_handle_history(const char *prompt)
{
    rt_kprintf("\033[2K\r%s%s", prompt, handle.line);
    return RT_FALSE;
}

/*
 * @brief 提交当前行到历史记录
 */
static void CLI_push_history(void)
{
    if (handle.line_position == 0)
        return;

    if (handle.history_count > 0)
    {
        if (rt_strncmp(handle.CLI_history[handle.history_count - 1], handle.line, CLI_CMD_BUFFER_SIZE) == 0)
        {
            handle.history_current = handle.history_count;
            return;
        }
    }

    if (handle.history_count >= CLI_HISTORY_LINES)
    {
        for (int index = 0; index < CLI_HISTORY_LINES - 1; index++)
        {
            rt_memcpy(handle.CLI_history[index],
                      handle.CLI_history[index + 1],
                      CLI_CMD_BUFFER_SIZE);
        }
        rt_memset(handle.CLI_history[CLI_HISTORY_LINES - 1], 0, CLI_CMD_BUFFER_SIZE);
        rt_memcpy(handle.CLI_history[CLI_HISTORY_LINES - 1], handle.line, handle.line_position);
        handle.history_count = CLI_HISTORY_LINES;
    }
    else
    {
        rt_memset(handle.CLI_history[handle.history_count], 0, CLI_CMD_BUFFER_SIZE);
        rt_memcpy(handle.CLI_history[handle.history_count], handle.line, handle.line_position);
        handle.history_count++;
    }
    handle.history_current = handle.history_count;
}

/*
 * @brief readline 实现（支持光标移动、历史、退格）
 * @param prompt      提示符
 * @param buffer      输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 输入行长度
 */
static int CLI_readline(const char *prompt, char *buffer, int buffer_size)
{
    rt_uint8_t ch;

start:
    rt_kprintf(prompt);

    while (1)
    {
        ch = CLI_getc();

        if (ch == 0x1b)
        {
            handle.stat = CLI_WAIT_SPEC_KEY;
            continue;
        }
        else if (handle.stat == CLI_WAIT_SPEC_KEY)
        {
            if (ch == 0x5b)
            {
                handle.stat = CLI_WAIT_FUNC_KEY;
                continue;
            }
            handle.stat = CLI_WAIT_NORMAL;
        }
        else if (handle.stat == CLI_WAIT_FUNC_KEY)
        {
            handle.stat = CLI_WAIT_NORMAL;
            switch (ch)
            {
                case 0x41:
                    if (handle.history_current > 0)
                    {
                        handle.history_current--;
                        rt_strncpy(handle.line, handle.CLI_history[handle.history_current], CLI_CMD_BUFFER_SIZE);
                        handle.line_position = rt_strlen(handle.line);
                        handle.line_curpos = handle.line_position;
                        CLI_handle_history(prompt);
                    }
                    continue;

                case 0x42:
                    if (handle.history_current < handle.history_count - 1)
                    {
                        handle.history_current++;
                        rt_strncpy(handle.line, handle.CLI_history[handle.history_current], CLI_CMD_BUFFER_SIZE);
                        handle.line_position = rt_strlen(handle.line);
                        handle.line_curpos = handle.line_position;
                        CLI_handle_history(prompt);
                    }
                    else
                    {
                        handle.history_current = handle.history_count;
                        rt_memset(handle.line, 0, CLI_CMD_BUFFER_SIZE);
                        handle.line_position = 0;
                        handle.line_curpos = 0;
                        CLI_handle_history(prompt);
                    }
                    continue;

                case 0x44:
                    if (handle.line_curpos > 0)
                    {
                        rt_kprintf("\033[D");
                        handle.line_curpos--;
                    }
                    continue;

                case 0x43:
                    if (handle.line_curpos < handle.line_position)
                    {
                        rt_kprintf("\033[C");
                        handle.line_curpos++;
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
            if (handle.line_curpos == 0)
                continue;

            handle.line_curpos--;
            handle.line_position--;

            rt_memmove(&handle.line[handle.line_curpos],
                       &handle.line[handle.line_curpos + 1],
                       handle.line_position - handle.line_curpos);
            handle.line[handle.line_position] = '\0';

            CLI_handle_history(prompt);
            continue;
        }

        if (ch == '\r' || ch == '\n')
        {
            CLI_push_history();
            rt_kprintf("\n");
            if (handle.line_position == 0)
            {
                goto start;
            }
            rt_strncpy(buffer, handle.line, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            int len = handle.line_position;

            rt_memset(handle.line, 0, CLI_CMD_BUFFER_SIZE);
            handle.line_curpos = handle.line_position = 0;
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

        if (handle.line_position >= (CLI_CMD_BUFFER_SIZE - 1))
        {
            continue;
        }

        if (handle.line_curpos < handle.line_position)
        {
            rt_memmove(&handle.line[handle.line_curpos + 1],
                       &handle.line[handle.line_curpos],
                       handle.line_position - handle.line_curpos);
        }
        handle.line[handle.line_curpos] = ch;
        handle.line_curpos++;
        handle.line_position++;
        handle.line[handle.line_position] = '\0';

        CLI_handle_history(prompt);
    }
}

/*
 * @brief CLI 工作线程：读取用户输入 -> 发送到 message hub -> 获取结果
 * @param p 线程参数（未使用）
 */
static void CLI_run(void *p)
{
    char input_buffer[CLI_CMD_BUFFER_SIZE] = {0};
    const char *device_name = RT_CONSOLE_DEVICE_NAME;
    Messages_t input_messages = RT_NULL;
    Messages_t output_message = RT_NULL;

    handle.thread_running = RT_TRUE;
    handle.device = rt_device_find(device_name);
    if (handle.device == RT_NULL)
    {
        LOG_D("The msh device find failed.");
        handle.thread_running = RT_FALSE;
        return;
    }

    handle.rx_indicate = handle.device->rx_indicate;
    rt_device_set_rx_indicate(handle.device, CLI_rxcb);

    rt_kprintf("\nPress CTRL+D to exit CLI shell.\n");

    while (handle.thread_running)
    {
        int length = CLI_readline("Enter command: ", input_buffer, sizeof(input_buffer));

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

            handle.message_hub->put_message(handle.message_hub,
                                            input_messages,
                                            handle.message_hub->input_mailbox);

            output_message = handle.message_hub->get_message(handle.message_hub,
                                                             handle.message_hub->output_mailbox);

            rt_kprintf("\n");
            messages_destroy(input_messages);
            messages_destroy(output_message);
        }

        rt_memset(input_buffer, 0, sizeof(input_buffer));
    }

    rt_device_set_rx_indicate(handle.device, handle.rx_indicate);
    if (handle.sem_inited)
    {
        rt_sem_detach(&(handle.rx_sem));
        handle.sem_inited = RT_FALSE;
    }
    handle.device = RT_NULL;
    rt_kprintf(FINSH_PROMPT);
    handle.thread_running = RT_FALSE;
}

/*
 * @brief CLI 通道初始化
 * @param message_hub 消息中心句柄
 * @param context     上下文管理器句柄
 * @return RT_EOK 成功
 */
int agent_cli_channel(MessageHub_t message_hub, Context_t context)
{
    if (handle.thread_running == RT_TRUE)
    {
        LOG_W("CLI channel already running!");
        return RT_EOK;
    }

    rt_memset(&handle, 0x00, sizeof(CLI_channel));
    handle.message_hub = message_hub;
    handle.stat = CLI_WAIT_NORMAL;

    rt_err_t sem_ret = rt_sem_init(&(handle.rx_sem), "CLI_rxsem", 0, RT_IPC_FLAG_FIFO);
    if (sem_ret != RT_EOK)
    {
        LOG_E("rx sem init failed");
        return sem_ret;
    }
    handle.sem_inited = RT_TRUE;

#if defined(RT_VERSION_CHECK) && (RTTHREAD_VERSION >= RT_VERSION_CHECK(5, 1, 0))
    rt_uint8_t prio = RT_SCHED_PRIV(rt_thread_self()).current_priority + 1;
#else
    rt_uint8_t prio = rt_thread_self()->current_priority + 1;
#endif

    rt_err_t result = rt_thread_init(&handle.thread,
                                     "CLI_channel",
                                     CLI_run, RT_NULL,
                                     handle.thread_stack, sizeof(handle.thread_stack),
                                     prio, 10);
    if (result != RT_EOK)
    {
        if (handle.sem_inited)
        {
            rt_sem_detach(&(handle.rx_sem));
            handle.sem_inited = RT_FALSE;
        }
        LOG_E("The CLI channel thread create failed.");
        return RT_ERROR;
    }
    rt_thread_startup(&handle.thread);

    return RT_EOK;
}