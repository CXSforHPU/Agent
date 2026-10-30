#include "utils/utils_internal.h"
#include "AgentRuntime.h"   /* agent_is_running / agent_get_message_hub */

#define LOG_TAG "Agent.utils.message"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 思考过程是否已开始输出（用于整段连续打印，避免逐片段刷日志） */
static rt_bool_t g_reasoning_started = RT_FALSE;

/*
 * @brief 打印思考过程
 * @note  推理模型的思考内容是逐片段流式返回的（每片段可能只有 1 个字符），
 *        若逐片段调 LOG_I 会把串口刷爆；这里首片段加前缀、其后连续追加，
 *        与正文流式输出保持一致的可读性
 */
void print_reasoning(const char *text)
{
    if (text == RT_NULL)
    {
        return;
    }

    if (!g_reasoning_started)
    {
        rt_kprintf("\n[thinking] ");
        g_reasoning_started = RT_TRUE;
    }
    rt_kprintf("%s", text);
}

/*
 * @brief 打印工具调用信息
 */
void print_tool_call(const char *text)
{
    LOG_I("Tool Call: \n%s", text);
}

/*
 * @brief 打印回复内容
 * @note  正文开始意味着本轮思考结束，复位标记使下一轮思考重新起头
 */
void print_context(const char *text)
{
    g_reasoning_started = RT_FALSE;
    rt_kprintf("%s", text);
}

/*
 * @brief 把一段文本作为用户消息注入 agent（等待 LLM 分析/总结/告警）
 * @param text 文本内容（非空）
 * @return RT_EOK 成功；RT_ERROR agent 未运行、消息中心未就绪或注入失败
 * @note  公共注入入口：供工具、驱动或其它内核模块把外部事件（MQTT 消息、
 *        传感器事件、文件到达等）主动交给 agent 处理，无需自己拼 MessageHub。
 *        - 消息以 role=user 的文本形式进入对话上下文；
 *        - put 成功后消息所有权移交 agent 主循环，调用方不得再释放；
 *        - agent 未运行（未执行 main_loop_entry / 已 cleanup）时返回 RT_ERROR，
 *          调用方应自行缓存或丢弃待注入内容；
 *        - 需要错峰时先判断 agent_is_busy()：agent 正在处理上一轮时投递会
 *          排到该轮结束之后（框架侧不会再合并/丢弃）。
 */
rt_err_t agent_inject_text(const char *text)
{
    MessageHub_t hub;
    Messages_t messages;

    if (text == RT_NULL || text[0] == '\0' || !agent_is_running())
    {
        return RT_ERROR;
    }

    hub = agent_get_message_hub();
    if (hub == RT_NULL)
    {
        return RT_ERROR;
    }

    messages = messages_create(1);
    if (messages == RT_NULL)
    {
        return RT_ERROR;
    }
    if (messages_append(messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(messages);
        return RT_ERROR;
    }

    /* 所有权移交：成功后由 agent 主循环消费并释放 */
    if (hub->put_message(hub, messages, hub->input_mailbox) != RT_EOK)
    {
        messages_destroy(messages);
        LOG_W("agent input mailbox full, drop injected message");
        return RT_ERROR;
    }

    return RT_EOK;
}
