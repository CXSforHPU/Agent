#include "AgentOutput.h"
#include "AgentRuntime.h"
#include "MessageHub.h"
#include "utils.h"

#define LOG_TAG "Agent.Output"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * @brief 向输出通道投递一段纯文本（工具循环兜底、空回答兜底用）
 * @param text 文本内容，RT_NULL/空串时用默认提示
 */
static void agent_put_text(const char *text)
{
    MessageHub_t hub = agent_get_message_hub();
    Messages_t messages;

    if (hub == RT_NULL)
    {
        return;
    }

    if (text == RT_NULL || text[0] == '\0')
    {
        text = "(no answer generated)";
    }

    messages = messages_create(1);
    if (messages == RT_NULL)
    {
        return;
    }
    if (messages_append(messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(messages);
        return;
    }
    if (hub->put_message(hub, messages, hub->output_mailbox) != RT_EOK)
    {
        LOG_W("output mailbox full, drop message");
        messages_destroy(messages);
    }
}

/*
 * @brief 兜底提示：既投递到输出 mailbox 供通道消费，也走流式打印路径
 * @note  CLI 通道不打印 mailbox 里的内容，只打印流式 on_context（见 channels/CLI.c），
 *        所以「请求失败」「工具循环超限」这类模型不会流式输出的文本必须两边都发
 */
void agent_notify(const char *text, void (*on_context)(const char *text))
{
    agent_put_text(text);
    if (on_context != RT_NULL)
    {
        on_context(text);
    }
}

/*
 * @brief 把一次 LLM 回复作为最终回答输出（无 tool_calls 或强制收尾时使用）
 * @param context   上下文管理器
 * @param resp      LLM 回复
 * @param on_context 正文打印回调（流式路径，通道可见）
 */
void agent_emit_answer(Context_t context, ChatResponse_t resp, void (*on_context)(const char *text))
{
    if (resp == RT_NULL)
    {
        return;
    }

    context->append_assistant_message(context, resp);

    if (resp->context == RT_NULL || resp->context[0] == '\0')
    {
        /*
         * 模型这次既没有工具调用也没有正文：必须给用户一句话，
         * 而且要走流式打印路径——CLI 通道收到输出 mailbox 的消息后只打印换行、
         * 不打印内容（见 channels/CLI.c 的 CLI_run），只投 mailbox 用户是看不到的。
         */
        agent_notify("(the model returned no answer; please restate your question)", on_context);
    }
    else
    {
        /* 正文已通过 on_context 流式打印过，这里只投 mailbox 供其它通道消费 */
        agent_put_text(resp->context);
#ifdef PKG_AGENT_TTS_AUTO_PLAY
        agent_tts_speak(resp->context);
#endif
    }
}
