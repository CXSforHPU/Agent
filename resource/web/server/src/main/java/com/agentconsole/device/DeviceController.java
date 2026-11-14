package com.agentconsole.device;

import com.agentconsole.a2a.WebSocketDeviceService;
import com.agentconsole.auth.JwtService;
import com.agentconsole.card.CapabilityCard;
import com.agentconsole.card.CapabilityCardService;
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

@RestController
@RequestMapping("/api/devices")
@Validated
public class DeviceController {

    /** 事件流的心跳间隔，秒。中间没事件也发一帧，免得代理把连接掐了。 */
    private static final long STREAM_KEEPALIVE_MS = 15000;

    private final DeviceService service;
    private final DeviceEventService events;
    private final ObjectMapper json;
    private final CapabilityCardService cards;

    private final WebSocketDeviceService ws;

    public DeviceController(DeviceService service, DeviceEventService events, ObjectMapper json,
                            WebSocketDeviceService ws, CapabilityCardService cards) {
        this.service = service;
        this.events = events;
        this.json = json;
        this.ws = ws;
        this.cards = cards;
    }

    public record WsConnection(long id, String name, String deviceId, boolean connected,
                               String status) {
    }

    public record WsStatus(boolean enabled, boolean listening, String bindAddress, int port,
                            List<WsConnection> devices) {
    }

    @GetMapping("/ws/status")
    public ApiResponse<WsStatus> wsStatus(JwtService.Claims claims) {
        List<WsConnection> devices = service.list(claims.userId()).stream()
                .filter(device -> device.address().startsWith("ws://"))
                .map(device -> new WsConnection(device.id(), device.name(),
                        device.address().substring(5), "online".equals(device.status()),
                        device.status()))
                .toList();
        return ApiResponse.ok(new WsStatus(ws.isEnabled(), true,
                "/api/a2a/ws", 0, devices));
    }

    /**
     * 登记设备。username/password 是设备激活凭据：密码只存摘要+盐，
     * 保存成功后返回一次性激活配置（账号、密码、设备ID），仅供配置板端，
     * 之后任何接口都不再回显密码。
     */
    public record DevicePayload(@NotBlank @Size(max = 64) String name,
                                @NotBlank @Size(max = 256) String address,
                                @Size(max = 64) String groupName,
                                @Size(max = 256) String note,
                                Boolean enabled,
                                @Size(max = 32) String username,
                                @Size(max = 128) String password) {
    }

    /** 一次性激活配置，只在创建/设置凭据的响应里出现一次。 */
    public record ActivationConfig(String deviceId, String username, String password) {
    }

    public record DeviceWithActivation(Device device, ActivationConfig activation) {
    }

    /** 设置/更换激活凭据的请求体。明文只在请求里出现，不落日志不回显。 */
    public record CredentialPayload(@NotBlank @Size(max = 32) String username,
                                    @NotBlank @Size(min = 6, max = 128) String password) {
    }

    public record ConfigPayload(@Size(max = 256) String apiKey,
                                @Size(max = 128) String modelName,
                                @Size(max = 256) String baseUrl) {
    }

    public record CardPayload(@NotBlank @Size(max = 64) String name,
                              @Size(max = 256) String description,
                              @Size(max = 32) String version) {
    }

    /** cardId 为具体卡=绑定；null=解绑（解绑要允许 null，所以不加 @NotNull）。 */
    public record BindCardPayload(Long cardId) {
    }

    @GetMapping
    public ApiResponse<List<Device>> list(JwtService.Claims claims) {
        return ApiResponse.ok(service.list(claims.userId()));
    }

    @PostMapping
    public ResponseEntity<ApiResponse<DeviceWithActivation>> create(
            @Valid @RequestBody DevicePayload body, JwtService.Claims claims) {
        try {
            Device created = service.create(claims.userId(), body.name(), body.address(),
                    body.groupName(), body.note(), body.username(), body.password());
            ActivationConfig activation = body.username() == null || body.username().isBlank()
                    ? null
                    : new ActivationConfig(created.address().substring(5),
                    body.username(), body.password());
            return ResponseEntity.status(HttpStatus.CREATED)
                    .body(ApiResponse.ok(new DeviceWithActivation(created, activation)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 给已有设备补/换激活凭据。已激活的设备返回 409，禁止换密码换 token。 */
    @PostMapping("/{id}/credential")
    public ResponseEntity<ApiResponse<DeviceWithActivation>> setCredential(
            @PathVariable long id, @Valid @RequestBody CredentialPayload body,
            JwtService.Claims claims) {
        try {
            Device device = service.get(id, claims.userId());
            Device updated = service.setCredential(device.id(), body.username(), body.password());
            return ResponseEntity.ok(ApiResponse.ok(new DeviceWithActivation(updated,
                    new ActivationConfig(updated.address().substring(5),
                            body.username(), body.password()))));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
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
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
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

    /**
     * 原「下发能力卡」接口。能力卡已改为绑定模式，这里明确返回不支持，
     * 不制造假成功；真正的能力卡管理在 /api/cards。
     */
    @PostMapping("/{id}/card")
    public ResponseEntity<ApiResponse<String>> pushCard(@PathVariable long id,
                                                        @Valid @RequestBody CardPayload body,
                                                        JwtService.Claims claims) {
        try {
            String result = service.pushCard(id, claims.userId(), body.name(),
                    body.description(), body.version());
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(result));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 查看这台设备绑定的能力卡。 */
    @GetMapping("/{id}/card")
    public ResponseEntity<ApiResponse<CapabilityCard>> boundCard(@PathVariable long id,
                                                                 JwtService.Claims claims) {
        try {
            Device device = service.get(id, claims.userId());
            if (device.cardId() == null) {
                return ResponseEntity.ok(ApiResponse.ok(null));
            }
            return ResponseEntity.ok(ApiResponse.ok(cards.get(device.cardId(), claims.userId())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 绑定能力卡。cardId 传 null 表示解绑。设备只能绑一张，换卡要先解绑。 */
    @PutMapping("/{id}/card")
    public ResponseEntity<ApiResponse<Device>> bindCard(@PathVariable long id,
                                                        @Valid @RequestBody BindCardPayload body,
                                                        JwtService.Claims claims) {
        try {
            Device device = service.get(id, claims.userId());
            if (body.cardId() == null) {
                // 解绑：已经没绑就原样返回
                if (device.cardId() != null) {
                    cards.unbindDevice(device.cardId(), claims.userId(), id);
                }
                return ResponseEntity.ok(ApiResponse.ok(service.get(id, claims.userId())));
            }
            cards.bindDevice(body.cardId(), claims.userId(), id);
            return ResponseEntity.ok(ApiResponse.ok(service.get(id, claims.userId())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
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
