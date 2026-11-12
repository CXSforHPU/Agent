package com.agentconsole.device;

import com.agentconsole.a2a.A2aClient;
import com.agentconsole.a2a.A2aModels;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;

@Service
public class DeviceService {

    private static final Logger log = LoggerFactory.getLogger(DeviceService.class);

    private final DeviceRepository repo;
    private final A2aClient a2a;
    private final HttpClient http;
    private final Duration httpTimeout;

    public DeviceService(DeviceRepository repo, A2aClient a2a,
                         @Value("${agent.a2a.connect-timeout-seconds:5}") long connectSeconds,
                         @Value("${agent.a2a.read-timeout-seconds:60}") long readSeconds) {
        this.repo = repo;
        this.a2a = a2a;
        this.httpTimeout = Duration.ofSeconds(readSeconds);
        this.http = HttpClient.newBuilder()
                .version(HttpClient.Version.HTTP_1_1)
                .connectTimeout(Duration.ofSeconds(connectSeconds))
                .followRedirects(HttpClient.Redirect.NORMAL)
                .build();
    }

    public List<Device> list(long userId) {
        return repo.findByUser(userId);
    }

    public Device get(long id, long userId) {
        return repo.findById(id, userId)
                .orElseThrow(() -> new IllegalArgumentException("设备不存在"));
    }

    public Device create(long userId, String name, String address, String groupName, String note) {
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("设备名不能为空");
        }
        if (address == null || address.isBlank()) {
            throw new IllegalArgumentException("地址不能为空");
        }
        return repo.create(userId, name.trim(), normalize(address), groupName, note);
    }

    public Device update(long id, long userId, String name, String address,
                         String groupName, String note, boolean enabled) {
        get(id, userId);
        if (!repo.update(id, userId, name, normalize(address), groupName, note, enabled)) {
            throw new IllegalArgumentException("设备不存在");
        }
        return get(id, userId);
    }

    public void delete(long id, long userId) {
        if (!repo.delete(id, userId)) {
            throw new IllegalArgumentException("设备不存在");
        }
    }

    /** 探活一次：拉能力卡，成功就算在线，顺带把卡片信息带回去。 */
    public ProbeResult probe(long id, long userId) {
        Device device = get(id, userId);
        try {
            A2aModels.AgentCard card = a2a.discover(device.address());
            repo.updateProbeResult(id, true);
            return new ProbeResult(true, card, null);
        } catch (Exception e) {
            repo.updateProbeResult(id, false);
            log.debug("设备 {} 探活失败：{}", device.name(), e.toString());
            return new ProbeResult(false, null, e.getMessage());
        }
    }

    /**
     * 配置下发：POST 到设备的 WebNet 通道 /config。
     * 只实现了 A2A 通道的设备没有这个端点，会返回失败而不是假装成功。
     */
    public String pushConfig(long id, long userId, String apiKey, String modelName, String baseUrl) {
        Device device = get(id, userId);
        var payload = new StringBuilder("{");
        boolean first = true;
        first = append(payload, first, "apiKey", apiKey);
        first = append(payload, first, "modelName", modelName);
        append(payload, first, "baseUrl", baseUrl);
        payload.append('}');

        HttpRequest request = HttpRequest.newBuilder(URI.create(normalize(device.address()) + "/config"))
                .timeout(httpTimeout)
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(payload.toString(), StandardCharsets.UTF_8))
                .build();
        try {
            HttpResponse<String> response = http.send(request,
                    HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
            if (response.statusCode() / 100 != 2) {
                return "设备返回 HTTP " + response.statusCode();
            }
            return "ok";
        } catch (Exception e) {
            return "配置下发失败：" + e.getMessage();
        }
    }

    private static boolean append(StringBuilder sb, boolean first, String key, String value) {
        if (value == null || value.isBlank()) {
            return first;
        }
        if (!first) {
            sb.append(',');
        }
        sb.append('"').append(key).append("\":\"")
                .append(value.replace("\\", "\\\\").replace("\"", "\\\""))
                .append('"');
        return false;
    }

    /** 定时探活所有启用的设备。 */
    @Scheduled(fixedDelayString = "${agent.a2a.probe-interval-ms:15000}")
    public void probeAll() {
        for (Device device : repo.findEnabled()) {
            try {
                a2a.discover(device.address());
                repo.updateProbeResult(device.id(), true);
            } catch (Exception e) {
                repo.updateProbeResult(device.id(), false);
            }
        }
    }

    public record ProbeResult(boolean online, A2aModels.AgentCard card, String error) {
    }

    private static String normalize(String address) {
        String s = address.trim();
        if (!s.startsWith("http://") && !s.startsWith("https://")) {
            s = "http://" + s;
        }
        return s;
    }
}
