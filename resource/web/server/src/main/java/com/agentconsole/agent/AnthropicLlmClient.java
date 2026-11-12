package com.agentconsole.agent;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * Anthropic 协议。请求体 {model, max_tokens, system, messages, tools}，
 * 鉴权用 x-api-key 头并带 anthropic-version，地址是 base_url 后面接 /v1/messages。
 * 与端侧 agent 包的 protocol_anthropic.c 是同一套协议。
 */
public class AnthropicLlmClient implements LlmClient {

    private final ObjectMapper json;
    private final HttpClient http;
    private final String baseUrl;
    private final String apiKey;
    private final String model;
    private final String anthropicVersion;
    private final int maxTokens;
    private final Duration timeout;

    public AnthropicLlmClient(ObjectMapper json, String baseUrl, String apiKey, String model,
                              String anthropicVersion, int maxTokens, Duration timeout) {
        this.json = json;
        this.http = HttpClient.newBuilder()
                .connectTimeout(Duration.ofSeconds(10))
                .followRedirects(HttpClient.Redirect.NORMAL)
                .build();
        this.baseUrl = baseUrl;
        this.apiKey = apiKey;
        this.model = model;
        this.anthropicVersion = anthropicVersion;
        this.maxTokens = maxTokens;
        this.timeout = timeout;
    }

    @Override
    public String protocol() {
        return "anthropic";
    }

    @Override
    public Reply complete(String system, List<Turn> turns, List<ToolSpec> tools) throws IOException {
        ObjectNode payload = json.createObjectNode();
        payload.put("model", model);
        payload.put("max_tokens", maxTokens);
        payload.put("stream", false);
        if (system != null && !system.isBlank()) {
            payload.put("system", system);
        }

        ArrayNode messages = payload.putArray("messages");
        for (Turn turn : turns) {
            switch (turn.role()) {
                case "assistant" -> {
                    ObjectNode node = messages.addObject();
                    node.put("role", "assistant");
                    ArrayNode content = node.putArray("content");
                    if (!turn.content().isEmpty()) {
                        content.addObject().put("type", "text").put("text", turn.content());
                    }
                    for (ToolCall call : turn.toolCalls()) {
                        ObjectNode use = content.addObject();
                        use.put("type", "tool_use");
                        use.put("id", call.id());
                        use.put("name", call.name());
                        use.set("input", json.readTree(call.arguments()));
                    }
                }
                case "tool" -> {
                    // Anthropic 的工具结果要放进一条 user 消息里
                    ObjectNode node = messages.addObject();
                    node.put("role", "user");
                    ObjectNode result = node.putArray("content").addObject();
                    result.put("type", "tool_result");
                    result.put("tool_use_id", turn.toolCallId());
                    result.put("content", turn.content());
                }
                default -> {
                    ObjectNode node = messages.addObject();
                    node.put("role", "user");
                    node.put("content", turn.content());
                }
            }
        }

        if (!tools.isEmpty()) {
            ArrayNode array = payload.putArray("tools");
            for (ToolSpec spec : tools) {
                ObjectNode item = array.addObject();
                item.put("name", spec.name());
                item.put("description", spec.description());
                item.set("input_schema", json.readTree(spec.parametersJson()));
            }
        }

        HttpRequest request = HttpRequest.newBuilder(URI.create(endpoint()))
                .timeout(timeout)
                .header("Content-Type", "application/json")
                .header("x-api-key", apiKey)
                .header("anthropic-version", anthropicVersion)
                .POST(HttpRequest.BodyPublishers.ofString(json.writeValueAsString(payload),
                        StandardCharsets.UTF_8))
                .build();
        HttpResponse<String> response;
        try {
            response = http.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("模型调用被中断", e);
        }
        if (response.statusCode() / 100 != 2) {
            throw new IllegalStateException("Anthropic 接口返回 HTTP " + response.statusCode()
                    + "：" + abbreviate(response.body()));
        }

        StringBuilder text = new StringBuilder();
        List<ToolCall> calls = new ArrayList<>();
        for (JsonNode block : json.readTree(response.body()).path("content")) {
            String type = block.path("type").asText();
            if ("text".equals(type)) {
                text.append(block.path("text").asText(""));
            } else if ("tool_use".equals(type)) {
                calls.add(new ToolCall(block.path("id").asText(), block.path("name").asText(),
                        json.writeValueAsString(block.path("input"))));
            }
        }
        return new Reply(text.toString(), calls);
    }

    /** base_url 后面接协议路径，结尾多余的斜杠去掉。 */
    private String endpoint() {
        String base = baseUrl.trim();
        while (base.endsWith("/")) {
            base = base.substring(0, base.length() - 1);
        }
        return base + "/v1/messages";
    }

    private static String abbreviate(String text) {
        if (text == null) {
            return "";
        }
        return text.length() > 300 ? text.substring(0, 300) + "…" : text;
    }
}
