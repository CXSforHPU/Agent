package com.agentconsole.chat;

import com.agentconsole.a2a.A2aClient;
import com.agentconsole.a2a.A2aModels;
import com.agentconsole.agent.WebsiteAgent;
import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import com.agentconsole.conversation.Conversation;
import com.agentconsole.conversation.ConversationRepository;
import com.agentconsole.conversation.Message;
import com.agentconsole.device.Device;
import com.agentconsole.device.DeviceRepository;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.validation.annotation.Validated;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.servlet.mvc.method.annotation.StreamingResponseBody;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * 聊天入口。deviceId 为空走网站侧 agent，否则经 A2A 转给端侧 agent。
 * 两个分支都把用户消息和回答落进会话历史。
 *
 * /chat 一次等到最终结果；/chat/stream 用 SSE 一路推中间过程，
 * 端侧 agent 的思考、工具调用、正文分片实时到网页，网页再发下一条。
 */
@RestController
@RequestMapping("/api/chat")
@Validated
public class ChatController {

    private final WebsiteAgent websiteAgent;
    private final A2aClient a2a;
    private final ConversationRepository conversations;
    private final DeviceRepository devices;
    private final ObjectMapper json;

    public ChatController(WebsiteAgent websiteAgent, A2aClient a2a,
                          ConversationRepository conversations, DeviceRepository devices,
                          ObjectMapper json) {
        this.websiteAgent = websiteAgent;
        this.a2a = a2a;
        this.conversations = conversations;
        this.devices = devices;
        this.json = json;
    }

    public record ChatPayload(@NotBlank @Size(max = 4096) String message,
                              Long conversationId,
                              Long deviceId) {
    }

    public record ChatResult(long conversationId, String agentKind, String reply) {
    }

    @PostMapping
    public ResponseEntity<ApiResponse<ChatResult>> chat(@Valid @RequestBody ChatPayload body,
                                                       JwtService.Claims claims) {
        Conversation conversation = null;
        String agentKind;
        String reply;
        List<Message> history = List.of();

        try {
            // 先复用已有会话取历史，没有就从空历史开始，等回复成功了再建
            if (body.conversationId() != null) {
                conversation = conversations.findById(body.conversationId(), claims.userId())
                        .orElseThrow(() -> new IllegalArgumentException("会话不存在"));
                history = conversations.findMessages(conversation.id());
            }

            if (body.deviceId() == null) {
                agentKind = "website";
                reply = websiteAgent.reply(claims.userId(), history, body.message());
            } else {
                agentKind = "edge";
                Device device = devices.findById(body.deviceId(), claims.userId())
                        .orElseThrow(() -> new IllegalArgumentException("设备不存在"));
                A2aModels.TaskResponse result = a2a.send(device.address(), body.message());
                reply = result.finalText();
                if (reply == null || reply.isBlank()) {
                    reply = "端侧 agent 没有返回内容（状态：" + statusOf(result) + "）";
                }
            }
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (Exception e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY)
                    .body(ApiResponse.fail("agent 调用失败：" + e.getMessage()));
        }

        // 回复拿到了才落库，失败的请求不留空会话
        if (conversation == null) {
            String title = agentKind.equals("edge") ? "端侧会话" : "网站侧会话";
            conversation = conversations.create(claims.userId(), body.deviceId(), agentKind, title);
        }
        conversations.appendMessage(conversation.id(), "user", body.message());
        conversations.appendMessage(conversation.id(), "assistant", reply);
        return ResponseEntity.ok(ApiResponse.ok(new ChatResult(conversation.id(), agentKind, reply)));
    }

    /**
     * 流式聊天。端侧 agent 的思考、工具调用、正文分片随到随推，
     * 最后再补一条 final，网页拿到的是完整答案。
     */
    @PostMapping(path = "/stream", produces = MediaType.TEXT_EVENT_STREAM_VALUE)
    public ResponseEntity<StreamingResponseBody> chatStream(@Valid @RequestBody ChatPayload payload,
                                                             JwtService.Claims claims) {
        Conversation conversation = null;
        List<Message> history = List.of();
        String deviceAddress = null;

        try {
            if (payload.conversationId() != null) {
                conversation = conversations.findById(payload.conversationId(), claims.userId())
                        .orElseThrow(() -> new IllegalArgumentException("会话不存在"));
                history = conversations.findMessages(conversation.id());
            }
            if (payload.deviceId() != null) {
                Device device = devices.findById(payload.deviceId(), claims.userId())
                        .orElseThrow(() -> new IllegalArgumentException("设备不存在"));
                deviceAddress = device.address();
            }
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest()
                    .body(out -> writeEvent(out, "error", e.getMessage()));
        }

        final Conversation current = conversation;
        final List<Message> context = history;
        final String address = deviceAddress;
        final boolean edge = address != null;

        StreamingResponseBody stream = out -> {
            try {
                if (!edge) {
                    writeEvent(out, "status", "网站侧 agent 处理中…");
                    String reply = websiteAgent.reply(claims.userId(), context, payload.message());
                    long conversationId = save(claims, current, payload, "website", reply);
                    writeEvent(out, "final", reply, conversationId);
                    return;
                }

                writeEvent(out, "status", "端侧 agent 处理中…");
                A2aModels.TaskResponse result = a2a.sendSubscribe(address, payload.message(),
                        event -> forward(out, event));
                String reply = result.finalText();
                if (reply == null || reply.isBlank()) {
                    reply = "端侧 agent 没有返回内容（状态：" + statusOf(result) + "）";
                }
                long conversationId = save(claims, current, payload, "edge", reply);
                writeEvent(out, "final", reply, conversationId);
            } catch (Exception e) {
                writeEvent(out, "error", "agent 调用失败：" + e.getMessage());
            }
        };

        return ResponseEntity.ok()
                .header("Cache-Control", "no-cache")
                .body(stream);
    }

    /** 把端侧 agent 的一帧状态转成网页能懂的事件。 */
    private void forward(OutputStream out, A2aModels.TaskResponse event) {
        try {
            if (event == null) {
                return;
            }
            if (event.artifacts() != null) {
                for (A2aModels.Artifact artifact : event.artifacts()) {
                    if (artifact == null || artifact.name() == null || artifact.name().isBlank()) {
                        continue;
                    }
                    /* 最终回答单独用 final 事件发，这里跳过 */
                    if ("response".equals(artifact.name())) {
                        continue;
                    }
                    writeEvent(out, artifact.name(), artifact.text());
                }
            }
            if (event.status() != null && "failed".equals(event.status().state())) {
                writeEvent(out, "error", "端侧 agent 执行失败");
            }
        } catch (IOException e) {
            /* 客户端断开就不再往下写 */
        }
    }

    /** 落库：回复拿到了才建会话，失败的请求不留空会话。 */
    private long save(JwtService.Claims claims, Conversation conversation,
                      ChatPayload payload, String agentKind, String reply) {
        Conversation target = conversation;
        if (target == null) {
            String title = agentKind.equals("edge") ? "端侧会话" : "网站侧会话";
            target = conversations.create(claims.userId(), payload.deviceId(), agentKind, title);
        }
        conversations.appendMessage(target.id(), "user", payload.message());
        conversations.appendMessage(target.id(), "assistant", reply);
        return target.id();
    }

    /** 写一条 SSE 事件。 */
    private void writeEvent(OutputStream out, String kind, String text) throws IOException {
        writeEvent(out, kind, text, 0);
    }

    private void writeEvent(OutputStream out, String kind, String text, long conversationId)
            throws IOException {
        ObjectNode node = json.createObjectNode();
        node.put("kind", kind);
        node.put("text", text == null ? "" : text);
        if (conversationId > 0) {
            node.put("conversationId", conversationId);
        }
        out.write(("data: " + json.writeValueAsString(node) + "\n\n")
                .getBytes(StandardCharsets.UTF_8));
        out.flush();
    }

    private static String statusOf(A2aModels.TaskResponse result) {
        return result.status() == null ? "unknown" : result.status().state();
    }
}
