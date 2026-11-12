package com.agentconsole.a2a;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;

/**
 * A2A 客户端。三个能力：发现能力卡、同步发任务、SSE 订阅任务。
 * 端侧 agent 的根地址存在 device.address，例如 http://192.168.1.10:8081。
 *
 * 同一台设备上的请求串行发：端侧 webnet 是单线程的，同时开两条连接时
 * 后到的那条会把先到那条的响应顶掉（QEMU user 网络下实测会直接丢响应）。
 */
@Component
public class A2aClient {

    private final ObjectMapper json;
    private final HttpClient http;
    private final Duration readTimeout;
    /** 一台设备一把锁，key 是去掉结尾斜杠的地址。 */
    private final Map<String, Object> deviceLocks = new ConcurrentHashMap<>();

    public A2aClient(ObjectMapper json,
                     @Value("${agent.a2a.connect-timeout-seconds:5}") long connectSeconds,
                     @Value("${agent.a2a.read-timeout-seconds:60}") long readSeconds) {
        this.json = json;
        this.readTimeout = Duration.ofSeconds(readSeconds);
        // 端侧 webnet 只讲 HTTP/1.1。JDK 默认 HTTP/2，明文时先发 Upgrade: h2c，
        // webnet 虽能忽略这个头，但没必要多走一遍升级协商。
        this.http = HttpClient.newBuilder()
                .version(HttpClient.Version.HTTP_1_1)
                .connectTimeout(Duration.ofSeconds(connectSeconds))
                .followRedirects(HttpClient.Redirect.NORMAL)
                .build();
    }

    /** 读能力卡，同时用作探活：能拿到 200 就认为节点在线。 */
    public A2aModels.AgentCard discover(String baseUrl) throws Exception {
        HttpRequest request = HttpRequest.newBuilder(URI.create(trim(baseUrl) + "/.well-known/agent.json"))
                .timeout(readTimeout)
                .header("Accept", "application/json")
                .GET()
                .build();
        HttpResponse<String> response = send(baseUrl, request);
        if (response.statusCode() / 100 != 2) {
            throw new IllegalStateException("能力卡返回 HTTP " + response.statusCode());
        }
        return json.readValue(response.body(), A2aModels.AgentCard.class);
    }

    /** 同步发任务，等最终结果。 */
    public A2aModels.TaskResponse send(String baseUrl, String text) throws Exception {
        String body = json.writeValueAsString(A2aModels.TaskRequest.of(text));
        HttpRequest request = HttpRequest.newBuilder(URI.create(trim(baseUrl) + "/tasks/send"))
                .timeout(readTimeout)
                .header("Content-Type", "application/json")
                .header("Accept", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8))
                .build();
        HttpResponse<String> response = send(baseUrl, request);
        if (response.statusCode() / 100 != 2) {
            throw new IllegalStateException("任务提交返回 HTTP " + response.statusCode() + "：" + abbreviate(response.body()));
        }
        return json.readValue(response.body(), A2aModels.TaskResponse.class);
    }

    /**
     * SSE 发任务，每收到一帧回调一次，返回最后一帧。
     * 事件里既有 status 变化也有 artifact 增量，调用方自己决定怎么拼。
     */
    public A2aModels.TaskResponse sendSubscribe(String baseUrl, String text,
                                                Consumer<A2aModels.TaskResponse> onEvent) throws Exception {
        String body = json.writeValueAsString(A2aModels.TaskRequest.of(text));
        HttpRequest request = HttpRequest.newBuilder(URI.create(trim(baseUrl) + "/tasks/sendSubscribe"))
                .timeout(readTimeout)
                .header("Content-Type", "application/json")
                .header("Accept", "text/event-stream")
                .POST(HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8))
                .build();

        synchronized (lockFor(baseUrl)) {
            HttpResponse<java.util.stream.Stream<String>> response =
                    http.send(request, HttpResponse.BodyHandlers.ofLines());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("流式任务返回 HTTP " + response.statusCode());
            }

            A2aModels.TaskResponse last = null;
            Iterator<String> lines = response.body().iterator();
            StringBuilder data = new StringBuilder();
            while (lines.hasNext()) {
                String line = lines.next();
                if (line.startsWith("data:")) {
                    data.append(line.substring(5).trim());
                    continue;
                }
                // 空行表示一帧结束
                if (line.isBlank() && !data.isEmpty()) {
                    A2aModels.TaskResponse event = json.readValue(data.toString(), A2aModels.TaskResponse.class);
                    if (onEvent != null) {
                        onEvent.accept(event);
                    }
                    last = event;
                    data.setLength(0);
                }
            }
            if (!data.isEmpty()) {
                A2aModels.TaskResponse event = json.readValue(data.toString(), A2aModels.TaskResponse.class);
                if (onEvent != null) {
                    onEvent.accept(event);
                }
                last = event;
            }
            if (last == null) {
                throw new IllegalStateException("SSE 流里没有收到任何事件");
            }
            return last;
        }
    }

    /** 发一个请求，同一台设备同一时刻只发一条。 */
    private HttpResponse<String> send(String baseUrl, HttpRequest request) throws Exception {
        synchronized (lockFor(baseUrl)) {
            return http.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        }
    }

    private Object lockFor(String baseUrl) {
        return deviceLocks.computeIfAbsent(trim(baseUrl), key -> new Object());
    }

    private static String trim(String baseUrl) {
        String s = baseUrl == null ? "" : baseUrl.trim();
        while (s.endsWith("/")) {
            s = s.substring(0, s.length() - 1);
        }
        return s;
    }

    private static String abbreviate(String s) {
        if (s == null) {
            return "";
        }
        return s.length() > 200 ? s.substring(0, 200) + "…" : s;
    }
}
