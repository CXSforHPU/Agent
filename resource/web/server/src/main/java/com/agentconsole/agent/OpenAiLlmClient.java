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
 * OpenAI 兼容协议。请求体 {model, messages, tools, max_tokens, stream}，
 * 鉴权用 Bearer 头，地址是 base_url 后面接 /chat/completions。
 * 与端侧 agent 包的 protocol_openai.c 是同一套协议。
 */
public class OpenAiLlmClient implements LlmClient {

    private final ObjectMapper json;
    private final HttpClient http;
    private final String baseUrl;
    private final String apiKey;
    private final String model;
    private final int maxTokens;
    private final Duration timeout;

    public OpenAiLlmClient(ObjectMapper json, String baseUrl, String apiKey, String model,
                           int maxTokens, Duration timeout) {
        this.json = json;
        this.http = HttpClient.newBuilder()
                .connectTimeout(Duration.ofSeconds(10))
                .followRedirects(HttpClient.Redirect.NORMAL)
                .build();
        this.baseUrl = baseUrl;
        this.apiKey = apiKey;
        this.model = model;
        this.maxTokens = maxTokens;
        this.timeout = timeout;
    }

    @Override
    public String protocol() {
        return "openai";
    }

    @Override
    public Reply complete(String system, List<Turn> turns, List<ToolSpec> tools) throws IOException {
        ObjectNode payload = json.createObjectNode();
        payload.put("model", model);
        payload.put("max_tokens", maxTokens);
        payload.put("stream", false);

        ArrayNode messages = payload.putArray("messages");
        if (system != null && !system.isBlank()) {
            messages.addObject().put("role", "system").put("content", system);
        }
        for (Turn turn : turns) {
            switch (turn.role()) {
                case "assistant" -> {
                    ObjectNode node = messages.addObject();
                    node.put("role", "assistant");
                    node.put("content", turn.content());
                    if (!turn.toolCalls().isEmpty()) {
                        ArrayNode calls = node.putArray("tool_calls");
                        for (ToolCall call : turn.toolCalls()) {
                            ObjectNode item = calls.addObject();
                            item.put("id", call.id());
                            item.put("type", "function");
                            ObjectNode function = item.putObject("function");
                            function.put("name", call.name());
                            function.put("arguments", call.arguments());
                        }
                    }
                }
                case "tool" -> {
                    ObjectNode node = messages.addObject();
                    node.put("role", "tool");
                    node.put("tool_call_id", turn.toolCallId());
                    node.put("content", turn.content());
                }
                default -> messages.addObject().put("role", "user").put("content", turn.content());
            }
        }

        if (!tools.isEmpty()) {
            ArrayNode array = payload.putArray("tools");
            for (ToolSpec spec : tools) {
                ObjectNode item = array.addObject();
                item.put("type", "function");
                ObjectNode function = item.putObject("function");
                function.put("name", spec.name());
                function.put("description", spec.description());
                function.set("parameters", json.readTree(spec.parametersJson()));
            }
        }

        HttpRequest request = HttpRequest.newBuilder(URI.create(endpoint()))
                .timeout(timeout)
                .header("Content-Type", "application/json")
                .header("Authorization", "Bearer " + apiKey)
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
            throw new IllegalStateException("OpenAI 接口返回 HTTP " + response.statusCode()
                    + "：" + abbreviate(response.body()));
        }

        JsonNode message = json.readTree(response.body()).path("choices").path(0).path("message");
        List<ToolCall> calls = new ArrayList<>();
        for (JsonNode item : message.path("tool_calls")) {
            JsonNode function = item.path("function");
            calls.add(new ToolCall(item.path("id").asText(), function.path("name").asText(),
                    function.path("arguments").asText("{}")));
        }
        return new Reply(message.path("content").asText(""), calls);
    }

    /** base_url 后面接协议路径，结尾多余的斜杠去掉。 */
    private String endpoint() {
        String base = baseUrl.trim();
        while (base.endsWith("/")) {
            base = base.substring(0, base.length() - 1);
        }
        return base + "/chat/completions";
    }

    private static String abbreviate(String text) {
        if (text == null) {
            return "";
        }
        return text.length() > 300 ? text.substring(0, 300) + "…" : text;
    }
}
