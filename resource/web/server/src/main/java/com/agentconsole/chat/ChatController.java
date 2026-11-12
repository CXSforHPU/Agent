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
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.validation.annotation.Validated;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.List;

/**
 * 聊天入口。deviceId 为空走网站侧 agent，否则经 A2A 转给端侧 agent。
 * 两个分支都把用户消息和回答落进会话历史。
 */
@RestController
@RequestMapping("/api/chat")
@Validated
public class ChatController {

    private final WebsiteAgent websiteAgent;
    private final A2aClient a2a;
    private final ConversationRepository conversations;
    private final DeviceRepository devices;

    public ChatController(WebsiteAgent websiteAgent, A2aClient a2a,
                          ConversationRepository conversations, DeviceRepository devices) {
        this.websiteAgent = websiteAgent;
        this.a2a = a2a;
        this.conversations = conversations;
        this.devices = devices;
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

    private static String statusOf(A2aModels.TaskResponse result) {
        return result.status() == null ? "unknown" : result.status().state();
    }
}
