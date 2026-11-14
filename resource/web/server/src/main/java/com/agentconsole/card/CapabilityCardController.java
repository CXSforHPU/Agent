package com.agentconsole.card;

import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import org.springframework.http.HttpStatus;
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

import java.util.List;

/**
 * 能力卡接口。全部按当前登录账号隔离：只能看到、改到自己创建的卡。
 */
@RestController
@RequestMapping("/api/cards")
@Validated
public class CapabilityCardController {

    private final CapabilityCardService service;

    public CapabilityCardController(CapabilityCardService service) {
        this.service = service;
    }

    public record CardPayload(@NotBlank @Size(max = 64) String name,
                              @Size(max = 512) String description,
                              @Size(max = 32) String version,
                              List<CapabilityCard.Skill> skills) {
    }

    public record BindPayload(@jakarta.validation.constraints.NotNull Long deviceId) {
    }

    @GetMapping
    public ApiResponse<List<CapabilityCard>> list(JwtService.Claims claims) {
        return ApiResponse.ok(service.list(claims.userId()));
    }

    @PostMapping
    public ResponseEntity<ApiResponse<CapabilityCard>> create(@Valid @RequestBody CardPayload body,
                                                              JwtService.Claims claims) {
        try {
            CapabilityCard card = service.create(claims.userId(), body.name(), body.description(),
                    body.version(), body.skills());
            return ResponseEntity.status(HttpStatus.CREATED).body(ApiResponse.ok(card));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @PutMapping("/{id}")
    public ResponseEntity<ApiResponse<CapabilityCard>> update(@PathVariable long id,
                                                              @Valid @RequestBody CardPayload body,
                                                              JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(service.update(id, claims.userId(),
                    body.name(), body.description(), body.version(), body.skills())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @DeleteMapping("/{id}")
    public ResponseEntity<ApiResponse<Void>> delete(@PathVariable long id,
                                                    JwtService.Claims claims) {
        try {
            service.delete(id, claims.userId());
            return ResponseEntity.ok(ApiResponse.ok());
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 绑定设备。设备已绑别的卡时返回 409，不静默换卡。 */
    @PostMapping("/{id}/bind")
    public ResponseEntity<ApiResponse<CapabilityCard>> bind(@PathVariable long id,
                                                            @Valid @RequestBody BindPayload body,
                                                            JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(
                    service.bindDevice(id, claims.userId(), body.deviceId())));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @PostMapping("/{id}/unbind")
    public ResponseEntity<ApiResponse<CapabilityCard>> unbind(@PathVariable long id,
                                                              @Valid @RequestBody BindPayload body,
                                                              JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(
                    service.unbindDevice(id, claims.userId(), body.deviceId())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }
}
