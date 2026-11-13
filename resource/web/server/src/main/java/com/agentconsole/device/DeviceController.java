package com.agentconsole.device;

import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.validation.annotation.Validated;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.servlet.mvc.method.annotation.StreamingResponseBody;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

@RestController
@RequestMapping("/api/devices")
@Validated
public class DeviceController {

    /** 事件流的心跳间隔，秒。中间没事件也发一帧，免得代理把连接掐了。 */
    private static final long STREAM_KEEPALIVE_MS = 15000;

    private final DeviceService service;
    private final DeviceEventService events;
    private final ObjectMapper json;

    public DeviceController(DeviceService service, DeviceEventService events, ObjectMapper json) {
        this.service = service;
        this.events = events;
        this.json = json;
    }

    public record DevicePayload(@NotBlank @Size(max = 64) String name,
                                @NotBlank @Size(max = 256) String address,
                                @Size(max = 64) String groupName,
                                @Size(max = 256) String note,
                                Boolean enabled) {
    }

    public record ConfigPayload(@Size(max = 256) String apiKey,
                                @Size(max = 128) String modelName,
                                @Size(max = 256) String baseUrl) {
    }

    public record CardPayload(@NotBlank @Size(max = 64) String name,
                              @Size(max = 256) String description,
                              @Size(max = 32) String version) {
    }

    @GetMapping
    public ApiResponse<List<Device>> list(JwtService.Claims claims) {
        return ApiResponse.ok(service.list(claims.userId()));
    }

    @PostMapping
    public ResponseEntity<ApiResponse<Device>> create(@Valid @RequestBody DevicePayload body,
                                                      JwtService.Claims claims) {
        try {
            Device created = service.create(claims.userId(), body.name(), body.address(),
                    body.groupName(), body.note());
            return ResponseEntity.status(HttpStatus.CREATED).body(ApiResponse.ok(created));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @PutMapping("/{id}")
    public ResponseEntity<ApiResponse<Device>> update(@PathVariable long id,
                                                      @Valid @RequestBody DevicePayload body,
                                                      JwtService.Claims claims) {
        try {
            boolean enabled = body.enabled() == null || body.enabled();
            return ResponseEntity.ok(ApiResponse.ok(service.update(id, claims.userId(), body.name(),
                    body.address(), body.groupName(), body.note(), enabled)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @DeleteMapping("/{id}")
    public ResponseEntity<ApiResponse<Void>> delete(@PathVariable long id, JwtService.Claims claims) {
        try {
            service.delete(id, claims.userId());
            return ResponseEntity.ok(ApiResponse.ok());
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 手动探活，同时返回能力卡。 */
    @PostMapping("/{id}/probe")
    public ResponseEntity<ApiResponse<DeviceService.ProbeResult>> probe(@PathVariable long id,
                                                                       JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(service.probe(id, claims.userId())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 配置下发，走设备的 WebNet 通道 /config。 */
    @PostMapping("/{id}/config")
    public ResponseEntity<ApiResponse<String>> pushConfig(@PathVariable long id,
                                                          @Valid @RequestBody ConfigPayload body,
                                                          JwtService.Claims claims) {
        try {
            String result = service.pushConfig(id, claims.userId(),
                    body.apiKey(), body.modelName(), body.baseUrl());
            if ("ok".equals(result)) {
                return ResponseEntity.ok(ApiResponse.ok(result));
            }
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(result));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 能力卡下发，走设备的 POST /card，板端存 SD 卡后重启仍然生效。 */
    @PostMapping("/{id}/card")
    public ResponseEntity<ApiResponse<String>> pushCard(@PathVariable long id,
                                                        @Valid @RequestBody CardPayload body,
                                                        JwtService.Claims claims) {
        try {
            String result = service.pushCard(id, claims.userId(), body.name(),
                    body.description(), body.version());
            if ("ok".equals(result)) {
                return ResponseEntity.ok(ApiResponse.ok(result));
            }
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(result));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /**
     * 板端上报入口。板端没有登录令牌，靠回调地址里的 token 认出是哪台设备，
     * 所以这个接口在过滤器里是放开的。
     */
    @PostMapping("/push/{token}")
    public ResponseEntity<ApiResponse<DeviceEvent>> receive(@PathVariable String token,
                                                            @RequestBody(required = false)
                                                            Map<String, String> body) {
        String kind = body == null ? null : body.get("kind");
        String text = body == null ? "" : body.get("text");
        try {
            return ResponseEntity.ok(ApiResponse.ok(events.receive(token, kind, text)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.status(HttpStatus.UNAUTHORIZED).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 这台设备最近的上报事件，新的在前。 */
    @GetMapping("/{id}/events")
    public ResponseEntity<ApiResponse<List<DeviceEvent>>> listEvents(@PathVariable long id,
                                                                     JwtService.Claims claims) {
        try {
            service.get(id, claims.userId());
            return ResponseEntity.ok(ApiResponse.ok(events.list(id, 50)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 事件流。板端一上报，网页这边立刻收到。 */
    @GetMapping(path = "/{id}/events/stream", produces = MediaType.TEXT_EVENT_STREAM_VALUE)
    public ResponseEntity<StreamingResponseBody> streamEvents(@PathVariable long id,
                                                              JwtService.Claims claims) {
        try {
            service.get(id, claims.userId());
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(out -> writeEvent(out, "error", e.getMessage()));
        }

        StreamingResponseBody stream = out -> {
            DeviceEventService.Subscription subscription = events.subscribe(id);
            try {
                for (;;) {
                    DeviceEvent event = subscription.poll(STREAM_KEEPALIVE_MS);
                    if (event == null) {
                        out.write(": keepalive\n\n".getBytes(StandardCharsets.UTF_8));
                        out.flush();
                        continue;
                    }
                    ObjectNode node = json.createObjectNode();
                    node.put("kind", event.kind());
                    node.put("text", event.text());
                    node.put("createdAt", event.createdAt());
                    out.write(("data: " + json.writeValueAsString(node) + "\n\n")
                            .getBytes(StandardCharsets.UTF_8));
                    out.flush();
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } catch (IOException e) {
                /* 客户端断开，不用管 */
            } finally {
                events.unsubscribe(id, subscription);
            }
        };

        return ResponseEntity.ok()
                .contentType(MediaType.TEXT_EVENT_STREAM)
                .header("Cache-Control", "no-cache")
                .body(stream);
    }

    /** 写一条 SSE 事件。 */
    private void writeEvent(OutputStream out, String kind, String text) throws IOException {
        ObjectNode node = json.createObjectNode();
        node.put("kind", kind);
        node.put("text", text == null ? "" : text);
        out.write(("data: " + json.writeValueAsString(node) + "\n\n")
                .getBytes(StandardCharsets.UTF_8));
        out.flush();
    }
}
