package com.agentconsole.common;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

import java.time.Instant;
import java.util.Map;

/**
 * 探活接口。setup.sh 用它判断后端是否起来，也用于前端启动时自检。
 */
@RestController
public class HealthController {

    @GetMapping("/api/health")
    public Map<String, Object> health() {
        return Map.of("status", "up", "time", Instant.now().toString());
    }
}
