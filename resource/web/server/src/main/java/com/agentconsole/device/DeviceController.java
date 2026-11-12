package com.agentconsole.device;

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

@RestController
@RequestMapping("/api/devices")
@Validated
public class DeviceController {

    private final DeviceService service;

    public DeviceController(DeviceService service) {
        this.service = service;
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
}
