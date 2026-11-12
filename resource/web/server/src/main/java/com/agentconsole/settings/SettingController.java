package com.agentconsole.settings;

import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

@RestController
@RequestMapping("/api/settings")
public class SettingController {

    private final SettingService service;

    public SettingController(SettingService service) {
        this.service = service;
    }

    /** 设置项定义 + 当前值，前端按这个渲染表单。defaults 是 rtconfig.h 给的默认值。 */
    public record SettingsView(Map<String, String> items, Map<String, String> labels,
                               Map<String, String> defaults) {
    }

    @GetMapping
    public ApiResponse<SettingsView> get(JwtService.Claims claims) {
        long userId = claims.userId();
        return ApiResponse.ok(new SettingsView(service.load(userId),
                Collections.unmodifiableMap(new LinkedHashMap<>(SettingService.KEYS)),
                service.rtDefaults(userId)));
    }

    @PutMapping
    public ResponseEntity<ApiResponse<Void>> save(@RequestBody Map<String, String> body,
                                                  JwtService.Claims claims) {
        try {
            service.save(claims.userId(), body);
            return ResponseEntity.ok(ApiResponse.ok());
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @PostMapping("/{key}/reset")
    public ResponseEntity<ApiResponse<Void>> reset(@PathVariable String key,
                                                   JwtService.Claims claims) {
        try {
            service.reset(claims.userId(), key);
            return ResponseEntity.ok(ApiResponse.ok());
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }
}
