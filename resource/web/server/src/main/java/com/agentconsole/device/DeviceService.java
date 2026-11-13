package com.agentconsole.device;

import com.agentconsole.a2a.A2aClient;
import com.agentconsole.a2a.A2aModels;
import com.agentconsole.ops.OpsService;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
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
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

@Service
public class DeviceService {

    private static final Logger log = LoggerFactory.getLogger(DeviceService.class);

    /** 回调地址多久重下一次。板端刷过固件以后 SD 卡上的地址会丢。 */
    private static final long PUSH_REFRESH_MS = 30 * 60 * 1000L;

    /**
     * 多久读一次板端当前的回调地址。
     *
     * <p>板端的 TCP 连接资源有限，每多一次探测就多一条连接要在 TIME_WAIT 里挂一分钟。
     * 读地址只是为了发现板端被刷过固件，间隔几分钟足够。
     */
    private static final long PUSH_READ_INTERVAL_MS = 5 * 60 * 1000L;

    private final DeviceRepository repo;
    private final DevicePushRepository pushes;
    private final A2aClient a2a;
    private final OpsService ops;
    private final int consolePort;
    private final HttpClient http;
    private final Duration httpTimeout;
    private final ObjectMapper json;
    /** 每块板子上次读回调地址的时间，用来给读动作限频。 */
    private final Map<String, Long> lastPushRead = new ConcurrentHashMap<>();

    public DeviceService(DeviceRepository repo, DevicePushRepository pushes, A2aClient a2a,
                         OpsService ops, ObjectMapper json,
                         @Value("${server.port:8080}") int consolePort,
                         @Value("${agent.a2a.connect-timeout-seconds:5}") long connectSeconds,
                         @Value("${agent.a2a.read-timeout-seconds:60}") long readSeconds) {
        this.repo = repo;
        this.pushes = pushes;
        this.a2a = a2a;
        this.ops = ops;
        this.json = json;
        this.consolePort = consolePort;
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
            ensurePushConfig(device, card);
            return new ProbeResult(true, card, null);
        } catch (Exception e) {
            repo.updateProbeResult(id, false);
            log.debug("设备 {} 探活失败：{}", device.name(), e.toString());
            return new ProbeResult(false, null, e.getMessage());
        }
    }

    /**
     * 把上报回调地址下发给板端。
     *
     * <p>只有能力卡声明了 pushNotifications 的设备才需要。板端只记最后下发的那个地址，
     * 所以回调按板子地址归到最早登记的那一行，多个账号登记同一台板子时也不会互相覆盖。
     * 下发前先读板端当前地址：板端刷过固件以后 SD 卡上的地址就没了，这时要重下。
     * 这个读动作按板子地址限频，避免把板端的连接资源占满。
     */
    private void ensurePushConfig(Device device, A2aModels.AgentCard card) {
        if (card == null || card.capabilities() == null
                || !Boolean.TRUE.equals(card.capabilities().pushNotifications())) {
            return;
        }
        Device owner = ownerOf(device);
        DevicePushConfig config = pushes.ensure(owner.id());
        String url = ops.pushCallbackUrl(config.token(), consolePort);

        String address = Device.normalizeAddress(device.address());
        boolean due = pushReadDue(address);
        String current = null;
        boolean readable = true;
        if (due) {
            try {
                current = a2a.readPushConfig(device.address()).url();
            } catch (Exception e) {
                readable = false;
                log.debug("读设备 {} 的回调地址失败：{}", device.name(), e.toString());
            }
        }

        boolean fresh = System.currentTimeMillis() - config.pushedAt() < PUSH_REFRESH_MS;
        boolean matches = due
                ? (readable ? url.equals(current) : (url.equals(config.url()) && fresh))
                : (url.equals(config.url()) && fresh);
        if (matches) {
            return;
        }
        try {
            a2a.setPushConfig(device.address(), url, String.valueOf(owner.id()));
            pushes.markPushed(owner.id(), url);
            log.debug("给设备 {} 下发的回调地址：{}", device.name(), url);
        } catch (Exception e) {
            log.debug("给设备 {} 下发回调地址失败：{}", device.name(), e.toString());
        }
    }

    /** 该不该现在读板端的回调地址：同一块板子最多 PUSH_READ_INTERVAL_MS 读一次。 */
    private boolean pushReadDue(String address) {
        long now = System.currentTimeMillis();
        Long last = lastPushRead.get(address);
        if (last != null && now - last < PUSH_READ_INTERVAL_MS) {
            return false;
        }
        lastPushRead.put(address, now);
        return true;
    }

    /**
     * 一块板子可能被几个账号各登记一次。板端的回调地址只有一份，
     * 归到最早登记的那一行，其余的行共用它的 token。
     */
    public Device ownerOf(Device device) {
        String address = Device.normalizeAddress(device.address());
        Device owner = device;
        for (Device candidate : repo.findAll()) {
            if (candidate.id() < owner.id()
                    && address.equals(Device.normalizeAddress(candidate.address()))) {
                owner = candidate;
            }
        }
        return owner;
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

    /**
     * 能力卡下发：POST 到设备的 /card。板端写进 SD 卡，重启后还是这份。
     *
     * <p>name 必填；description 与 version 留空就保持板端当前的值。
     * 下发的是 A2A 通道的端点，WebNet 通道的设备没有这个接口。
     */
    public String pushCard(long id, long userId, String name, String description, String version) {
        Device device = get(id, userId);
        if (name == null || name.isBlank()) {
            return "能力卡名字不能为空";
        }

        ObjectNode payload = json.createObjectNode();
        payload.put("name", name.trim());
        if (description != null && !description.isBlank()) {
            payload.put("description", description.trim());
        }
        if (version != null && !version.isBlank()) {
            payload.put("version", version.trim());
        }

        String body;
        try {
            body = json.writeValueAsString(payload);
        } catch (JsonProcessingException e) {
            return "能力卡序列化失败：" + e.getMessage();
        }

        HttpRequest request = HttpRequest.newBuilder(URI.create(normalize(device.address()) + "/card"))
                .timeout(httpTimeout)
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8))
                .build();
        try {
            HttpResponse<String> response = http.send(request,
                    HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
            if (response.statusCode() / 100 != 2) {
                String detail = response.body() == null ? "" : response.body();
                if (detail.length() > 200) {
                    detail = detail.substring(0, 200) + "…";
                }
                return "设备返回 HTTP " + response.statusCode() + "：" + detail;
            }
            return "ok";
        } catch (Exception e) {
            return "能力卡下发失败：" + e.getMessage();
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
        /* 同一块板子只探一次：端侧 webnet 是单线程的，重复探活会把它的连接占满。 */
        Map<String, List<Device>> byAddress = new LinkedHashMap<>();
        for (Device device : repo.findEnabled()) {
            byAddress.computeIfAbsent(Device.normalizeAddress(device.address()),
                    key -> new ArrayList<>()).add(device);
        }
        for (List<Device> group : byAddress.values()) {
            Device first = group.get(0);
            try {
                A2aModels.AgentCard card = a2a.discover(first.address());
                for (Device device : group) {
                    repo.updateProbeResult(device.id(), true);
                }
                ensurePushConfig(first, card);
            } catch (Exception e) {
                for (Device device : group) {
                    repo.updateProbeResult(device.id(), false);
                }
            }
        }
    }

    public record ProbeResult(boolean online, A2aModels.AgentCard card, String error) {
    }

    private static String normalize(String address) {
        return Device.normalizeAddress(address);
    }
}
