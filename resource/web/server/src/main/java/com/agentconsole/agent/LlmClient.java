package com.agentconsole.agent;

import java.io.IOException;
import java.util.List;

/**
 * 网站侧 agent 的模型调用接口。OpenAI 与 Anthropic 两套协议各一个实现，
 * 对外的对话结构、工具契约保持一致，端侧 agent 包的 protocol_*.c 是同样的协议。
 */
public interface LlmClient {

    /** 协议名，openai 或 anthropic。 */
    String protocol();

    /**
     * 跑一轮对话。
     *
     * @param system 系统提示词
     * @param turns  历史对话，最后一条是本轮用户输入
     * @param tools  可用工具
     * @return 模型回复，text 是正文，toolCalls 是模型要调的工具
     */
    Reply complete(String system, List<Turn> turns, List<ToolSpec> tools) throws IOException;

    /**
     * 一条对话记录。
     *
     * @param role       user / assistant / tool
     * @param content    文本内容
     * @param toolCalls  assistant 轮里模型发起的工具调用
     * @param toolCallId tool 轮对应的调用 id
     */
    record Turn(String role, String content, List<ToolCall> toolCalls, String toolCallId) {

        public static Turn user(String content) {
            return new Turn("user", content == null ? "" : content, List.of(), null);
        }

        public static Turn assistant(String content, List<ToolCall> toolCalls) {
            return new Turn("assistant", content == null ? "" : content,
                    toolCalls == null ? List.of() : toolCalls, null);
        }

        public static Turn tool(String content, String toolCallId) {
            return new Turn("tool", content == null ? "" : content, List.of(), toolCallId);
        }
    }

    /** 模型发起的一次工具调用。 */
    record ToolCall(String id, String name, String arguments) {
    }

    /** 模型回复。 */
    record Reply(String text, List<ToolCall> toolCalls) {

        public static Reply of(String text) {
            return new Reply(text == null ? "" : text, List.of());
        }
    }

    /** 工具定义，参数用 JSON Schema 描述。 */
    record ToolSpec(String name, String description, String parametersJson) {
    }
}
