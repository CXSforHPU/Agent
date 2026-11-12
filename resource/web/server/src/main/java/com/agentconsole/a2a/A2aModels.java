package com.agentconsole.a2a;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.fasterxml.jackson.annotation.JsonProperty;

import java.util.List;
import java.util.Map;

/**
 * A2A 协议的数据结构，字段名与协议一致，直接交给 Jackson 反序列化。
 * 见 https://haozhe-xing.github.io/agent_learning/zh/chapter_19_protocol/02_a2a_protocol.html
 */
public final class A2aModels {

    private A2aModels() {
    }

    /** 消息里的一个内容块。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Part(String type, String text) {
        public static Part text(String text) {
            return new Part("text", text);
        }
    }

    /** A2A 消息：role 取 user 或 agent。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Message(String role, List<Part> parts) {
        public static Message user(String text) {
            return new Message("user", List.of(Part.text(text)));
        }

        /** 把所有文本块拼成一段，方便落库和展示。 */
        public String text() {
            if (parts == null) {
                return "";
            }
            StringBuilder sb = new StringBuilder();
            for (Part p : parts) {
                if (p != null && p.text() != null) {
                    sb.append(p.text());
                }
            }
            return sb.toString();
        }
    }

    /** 提交任务的请求体。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record TaskRequest(String id, Message message) {
        public static TaskRequest of(String text) {
            return new TaskRequest(null, Message.user(text));
        }
    }

    /** 任务状态。state 取 submitted / working / input-required / completed / failed / canceled。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Status(String state, String message, String timestamp) {
        public boolean isTerminal() {
            return "completed".equals(state) || "failed".equals(state) || "canceled".equals(state);
        }
    }

    /** 任务产出。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Artifact(List<Part> parts, String name,
                           @JsonProperty("append") Boolean append,
                           @JsonProperty("lastChunk") Boolean lastChunk) {
        public String text() {
            if (parts == null) {
                return "";
            }
            StringBuilder sb = new StringBuilder();
            for (Part p : parts) {
                if (p != null && p.text() != null) {
                    sb.append(p.text());
                }
            }
            return sb.toString();
        }
    }

    /** 任务响应。同步接口返回完整结果，SSE 每一帧也用这个结构。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record TaskResponse(String id, Status status, List<Artifact> artifacts) {
        /** 取最后一个文本产出，通常是最终回答。 */
        public String finalText() {
            if (artifacts == null || artifacts.isEmpty()) {
                return "";
            }
            return artifacts.get(artifacts.size() - 1).text();
        }
    }

    /** 能力声明里的 capabilities 段。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Capabilities(Boolean streaming, Boolean pushNotifications,
                               @JsonProperty("stateTransitionHistory") Boolean stateHistory) {
    }

    /** 能力卡里的一个技能。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Skill(String id, String name, String description,
                        @JsonProperty("inputModes") List<String> inputModes,
                        @JsonProperty("outputModes") List<String> outputModes) {
    }

    /** /.well-known/agent.json。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record AgentCard(String name, String description, String url, String version,
                            Capabilities capabilities,
                            @JsonProperty("defaultInputModes") List<String> inputModes,
                            @JsonProperty("defaultOutputModes") List<String> outputModes,
                            List<Skill> skills,
                            @JsonProperty("additionalInterfaces") List<Map<String, String>> interfaces) {
    }
}
