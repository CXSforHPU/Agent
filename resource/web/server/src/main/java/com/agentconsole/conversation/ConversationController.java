package com.agentconsole.conversation;

import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.List;

/**
 * 会话历史：列表、详情（含全部消息）、新建、删除。
 * 会话归属用户，device_id 可空表示网站侧 agent 的会话。
 */
@RestController
@RequestMapping("/api/conversations")
public class ConversationController {

    private final ConversationRepository repo;

    public ConversationController(ConversationRepository repo) {
        this.repo = repo;
    }

    public record CreatePayload(Long deviceId, @jakarta.validation.constraints.Size(max = 64) String title) {
    }

    public record ConversationDetail(Conversation conversation, List<Message> messages) {
    }

    @GetMapping
    public ApiResponse<List<Conversation>> list(@RequestParam(required = false) Long deviceId,
                                                JwtService.Claims claims) {
        List<Conversation> all = repo.findByUser(claims.userId());
        if (deviceId == null) {
            return ApiResponse.ok(all);
        }
        return ApiResponse.ok(all.stream().filter(c -> deviceId.equals(c.deviceId())).toList());
    }

    @GetMapping("/{id}")
    public ResponseEntity<ApiResponse<ConversationDetail>> detail(@PathVariable long id,
                                                                 JwtService.Claims claims) {
        return repo.findById(id, claims.userId())
                .map(c -> ApiResponse.ok(new ConversationDetail(c, repo.findMessages(id))))
                .map(ResponseEntity::ok)
                .orElseGet(() -> ResponseEntity.badRequest().body(ApiResponse.fail("会话不存在")));
    }

    @PostMapping
    public ResponseEntity<ApiResponse<Conversation>> create(@RequestBody CreatePayload body,
                                                           JwtService.Claims claims) {
        String agentKind = body.deviceId() == null ? "website" : "edge";
        String title = body.title() == null || body.title().isBlank() ? "新会话" : body.title();
        Conversation created = repo.create(claims.userId(), body.deviceId(), agentKind, title);
        return ResponseEntity.status(HttpStatus.CREATED).body(ApiResponse.ok(created));
    }

    @DeleteMapping("/{id}")
    public ResponseEntity<ApiResponse<Void>> delete(@PathVariable long id, JwtService.Claims claims) {
        if (!repo.delete(id, claims.userId())) {
            return ResponseEntity.badRequest().body(ApiResponse.fail("会话不存在"));
        }
        return ResponseEntity.ok(ApiResponse.ok());
    }

    /** 供聊天服务复用的追加接口，不直接暴露给前端。 */
    public Message append(long conversationId, String role, String content) {
        return repo.appendMessage(conversationId, role, content);
    }

    public Conversation get(long id, long userId) {
        return repo.findById(id, userId).orElseThrow(() -> new IllegalArgumentException("会话不存在"));
    }

    public void rename(long id, String title) {
        repo.rename(id, title);
    }
}
