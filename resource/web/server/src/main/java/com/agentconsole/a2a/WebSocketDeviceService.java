package com.agentconsole.a2a;

import com.agentconsole.auth.PasswordHasher;
import com.agentconsole.card.CapabilityCard;
import com.agentconsole.card.CapabilityCardRepository;
import com.agentconsole.device.Device;
import com.agentconsole.device.DeviceEventService;
import com.agentconsole.device.DeviceRepository;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.socket.*;
import org.springframework.web.socket.handler.TextWebSocketHandler;
import org.springframework.web.socket.config.annotation.*;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;

/**
 * 设备侧 WebSocket 服务：板端主动连入。账号密码只在激活这一次出现，
 * 通过后签发一次性设备 token，之后板端凭 token 重连。
 *
 * <p>一台设备只激活一次，冲突明确拒绝、不发新 token：
 * <ul>
 *   <li>同 ID 已在线 → already_online，拒绝重复连接，重复激活无从发生；</li>
 *   <li>已有激活记录再拿密码来 → already_activated，禁止换密码换 token；</li>
 *   <li>两个并发激活撞车 → 条件更新 + device_token 唯一索引兜底，输的一方 already_activated。</li>
 * </ul>
 *
 * <p>权限边界：会话只挂在 device 行上，账号密码是设备 credential，
 * 与用户登录（JWT）无关；设备删除后行消失，允许重新登记并再次激活。
 */
@Configuration
@EnableWebSocket
public class WebSocketDeviceService extends TextWebSocketHandler implements WebSocketConfigurer {

    /** 激活/鉴权错误码，板端按 code 打印未激活或冲突原因。 */
    public static final String ERR_ALREADY_ONLINE = "already_online";
    public static final String ERR_ALREADY_ACTIVATED = "already_activated";
    public static final String ERR_BAD_CREDENTIAL = "bad_credential";
    public static final String ERR_NOT_FOUND = "device_not_found";
    public static final String ERR_BAD_TOKEN = "bad_token";

    private static final int TOKEN_BYTES = 32;

    private final ObjectMapper json;
    private final DeviceEventService events;
    private final DeviceRepository devices;
    private final CapabilityCardRepository cards;
    private final long timeout;
    private final SecureRandom random = new SecureRandom();

    private final ConcurrentMap<String, Session> sessions = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, WebSocketSession> connections = new ConcurrentHashMap<>();
    private final ScheduledExecutorService timer = Executors.newScheduledThreadPool(2, r -> {
        Thread t = new Thread(r, "a2a-ws-deadline");
        t.setDaemon(true);
        return t;
    });

    public WebSocketDeviceService(ObjectMapper json, DeviceEventService events,
                                  DeviceRepository devices, CapabilityCardRepository cards,
                                  @Value("${agent.a2a.read-timeout-seconds:60}") long timeout) {
        this.json = json;
        this.events = events;
        this.devices = devices;
        this.cards = cards;
        this.timeout = timeout;
    }

    @PostConstruct
    public void start() {
        if (timeout < 1) {
            throw new IllegalStateException("Invalid A2A deadline");
        }
        // 心跳：空闲每 20s 一帧 ping；timeout+30 秒没收任何帧就断开，状态即时变离线
        timer.scheduleWithFixedDelay(() -> sessions.values().forEach(s -> {
            if (System.nanoTime() - s.lastReceive > TimeUnit.SECONDS.toNanos(timeout + 30)) {
                closeSocket(s.socket);
            } else if (!s.busy) {
                try {
                    s.write(json.createObjectNode().put("type", "ping")
                            .put("id", UUID.randomUUID().toString()));
                } catch (Exception e) {
                    closeSocket(s.socket);
                }
            }
        }), 20, 20, TimeUnit.SECONDS);
    }

    @Override
    public void registerWebSocketHandlers(WebSocketHandlerRegistry registry) {
        registry.addHandler(this, "/api/a2a/ws");
    }

    @Override
    public void afterConnectionEstablished(WebSocketSession socket) {
        if (connections.size() >= 32) {
            closeSocket(socket);
            return;
        }
        socket.setTextMessageSizeLimit(8192);
        socket.setBinaryMessageSizeLimit(8192);
        connections.put(socket.getId(), socket);
        // 5 秒内必须完成 hello，否则按未认证断开
        timer.schedule(() -> {
            if (!socket.getAttributes().containsKey("device")) {
                closeSocket(socket);
            }
        }, 5, TimeUnit.SECONDS);
    }

    @Override
    protected void handleTextMessage(WebSocketSession socket, TextMessage message) {
        try {
            JsonNode frame = json.readTree(message.getPayload());
            if (frame == null || !frame.isObject()) {
                throw new IOException();
            }
            Session s = (Session) socket.getAttributes().get("device");
            if (s == null) {
                authenticate(socket, frame);
                return;
            }
            s.lastReceive = System.nanoTime();
            if ("report".equals(frame.path("type").asText())) {
                events.receiveAddress("ws://" + s.id, frame.path("kind").asText("report"),
                        frame.path("text").asText());
                return;
            }
            CompletableFuture<JsonNode> pending = s.pending.get(frame.path("id").asText());
            if (pending != null) {
                pending.complete(frame);
            }
        } catch (Exception e) {
            closeSocket(socket);
        }
    }

    /** hello 鉴权：账号密码激活（换新 token）或 token 重连。失败回错误码后断开。 */
    private void authenticate(WebSocketSession socket, JsonNode frame) throws IOException {
        if (!"hello".equals(frame.path("type").asText())) {
            reject(socket, ERR_BAD_TOKEN, "expect hello");
            return;
        }
        String deviceId = frame.path("deviceId").asText("");
        if (!deviceId.matches("[A-Za-z0-9_-]{1,64}")) {
            reject(socket, ERR_BAD_TOKEN, "bad device id");
            return;
        }
        // 同 ID 已在线：拒绝第二次连接，重复激活也就无从发生
        if (sessions.containsKey(deviceId)) {
            reject(socket, ERR_ALREADY_ONLINE, "device already online");
            return;
        }

        Device device;
        String issuedToken = null;
        String username = frame.path("username").asText("");
        String password = frame.path("password").asText("");
        String token = frame.path("token").asText("");

        if (!username.isEmpty() || !password.isEmpty()) {
            // 账号密码激活。用户名全局唯一（idx_device_cred_username），找到行即目标设备
            Optional<Device> target = devices.findByCredentialUsername(username);
            if (target.isEmpty() || !("ws://" + deviceId).equals(target.get().address())) {
                reject(socket, ERR_NOT_FOUND, "device not registered for this credential");
                return;
            }
            device = target.get();
            if (device.activation() == null || "unactivated".equals(device.activation())) {
                reject(socket, ERR_NOT_FOUND, "device has no activation credential");
                return;
            }
            Optional<DeviceRepository.Credential> credential = devices.findCredential(device.id());
            if (credential.isEmpty() || !PasswordHasher.matches(password, credential.get().salt(),
                    credential.get().hash())) {
                reject(socket, ERR_BAD_CREDENTIAL, "bad username or password");
                return;
            }
            // 已有激活记录：禁止换密码换 token，明确冲突
            if (devices.isActivated(device.id())) {
                reject(socket, ERR_ALREADY_ACTIVATED, "device already activated; token unchanged");
                return;
            }
            issuedToken = newToken();
            if (!devices.activate(device.id(), issuedToken)) {
                // 并发二次激活被条件更新 / 唯一索引拦下
                reject(socket, ERR_ALREADY_ACTIVATED, "device already activated; token unchanged");
                return;
            }
            device = devices.findById(device.id(), device.userId()).orElse(device);
        } else if (!token.isEmpty()) {
            // token 重连：只认库里签发过的 token
            Optional<Device> target = devices.findByToken(token);
            if (target.isEmpty() || !("ws://" + deviceId).equals(target.get().address())) {
                reject(socket, ERR_BAD_TOKEN, "unknown device token");
                return;
            }
            device = target.get();
        } else {
            reject(socket, ERR_BAD_TOKEN, "hello needs username/password or token");
            return;
        }

        Session session = new Session(deviceId, socket);
        if (sessions.putIfAbsent(deviceId, session) != null) {
            reject(socket, ERR_ALREADY_ONLINE, "device already online");
            return;
        }
        socket.getAttributes().put("device", session);

        ObjectNode welcome = json.createObjectNode();
        welcome.put("type", "welcome");
        welcome.put("deviceId", deviceId);
        if (issuedToken != null) {
            // 只在激活这一次下发新 token，重连不重复发
            welcome.put("token", issuedToken);
        }
        session.write(welcome);

        // 激活/重连成功后，把绑定的能力卡内容推给板端（板端回 cardResult）
        pushBoundCard(device, session);
    }

    /** 绑定了能力卡就推给板端；没绑定不发，不影响会话。 */
    private void pushBoundCard(Device device, Session session) {
        if (device.cardId() == null) {
            return;
        }
        try {
            Optional<CapabilityCard> card = cards.findById(device.cardId(), device.userId());
            if (card.isEmpty()) {
                return;
            }
            ObjectNode frame = json.createObjectNode();
            frame.put("type", "card");
            frame.put("id", UUID.randomUUID().toString());
            frame.set("card", json.valueToTree(card.get()));
            // fire-and-forget：板端回的 cardResult 没有 pending，会被正常丢弃
            session.write(frame);
        } catch (Exception ignored) {
            /* 推卡失败不影响已建立的会话 */
        }
    }

    /**
     * 设备绑定/换卡后立刻把新卡推给在线的板端；离线设备在下次 welcome 时也会收到。
     * 没连着就什么都不做，绑定关系已落库，不丢。
     */
    public void pushCard(String address) {
        if (address == null || !address.matches("ws://[A-Za-z0-9_-]{1,64}")) {
            return;
        }
        Session session = sessions.get(deviceId(address));
        if (session == null) {
            return;
        }
        try {
            for (Device device : devices.findByAddress(address)) {
                if (device.cardId() != null) {
                    pushBoundCard(device, session);
                    return;
                }
            }
        } catch (Exception ignored) {
            /* 推送失败不影响绑定结果 */
        }
    }

    /** 回错误码帧后断开。板端按 code 打印已激活/未激活原因。 */
    private void reject(WebSocketSession socket, String code, String message) {
        try {
            ObjectNode frame = json.createObjectNode();
            frame.put("type", "error");
            frame.put("code", code);
            frame.put("message", message);
            socket.sendMessage(new TextMessage(json.writeValueAsString(frame)));
        } catch (Exception ignored) {
            /* 尽力送达 */
        } finally {
            closeSocket(socket);
        }
    }

    @Override
    public void afterConnectionClosed(WebSocketSession socket, CloseStatus status) {
        connections.remove(socket.getId());
        Session s = (Session) socket.getAttributes().get("device");
        if (s != null) {
            sessions.remove(s.id, s);
            s.pending.values().forEach(f -> f.completeExceptionally(
                    new IOException("Device disconnected; outcome unknown")));
        }
    }

    @Override
    public void handleTransportError(WebSocketSession socket, Throwable e) {
        closeSocket(socket);
    }

    private void closeSocket(WebSocketSession socket) {
        try {
            socket.close();
        } catch (Exception ignored) {
        }
    }

    /** 逻辑地址 ws://设备ID 里的设备ID；设备表 address 列就是这个格式。 */
    public static String deviceId(String address) {
        if (address == null || !address.matches("ws://[A-Za-z0-9_-]{1,64}")) {
            throw new IllegalArgumentException("设备地址必须是 ws://设备ID；这是逻辑标识，不是网络 URL");
        }
        return address.substring(5);
    }

    public boolean isConnected(String address) {
        if (address == null || !address.matches("ws://[A-Za-z0-9_-]{1,64}")) {
            return false;
        }
        Session s = sessions.get(deviceId(address));
        return s != null && s.socket.isOpen();
    }

    /** WebSocket 通道始终可用：激活由设备表凭据驱动，不再依赖独立 token 配置。 */
    public boolean isEnabled() {
        return true;
    }

    public JsonNode request(String address, String type, String text) throws Exception {
        Session s = sessions.get(deviceId(address));
        if (s == null) {
            throw new IOException("WebSocket device offline");
        }
        synchronized (s) {
            s.busy = true;
            String id = UUID.randomUUID().toString();
            ObjectNode frame = json.createObjectNode().put("type", type).put("id", id);
            if (text != null) {
                frame.put("text", text);
            }
            CompletableFuture<JsonNode> future = new CompletableFuture<>();
            s.pending.put(id, future);
            try {
                s.write(frame);
                JsonNode result = future.get(timeout, TimeUnit.SECONDS);
                if (result.has("error")) {
                    throw new IOException(result.path("error").asText());
                }
                if (!(type + "Result").equals(result.path("type").asText())) {
                    throw new IOException("Unexpected response");
                }
                return result;
            } catch (Exception e) {
                closeSocket(s.socket);
                throw e;
            } finally {
                s.pending.remove(id);
                s.busy = false;
            }
        }
    }

    public A2aModels.AgentCard discover(String address) throws Exception {
        return json.treeToValue(request(address, "card", null).path("card"),
                A2aModels.AgentCard.class);
    }

    public A2aModels.TaskResponse send(String address, String text) throws Exception {
        JsonNode result = request(address, "task", text);
        return new A2aModels.TaskResponse(result.path("id").asText(),
                new A2aModels.Status(result.path("state").asText("failed"), null, null),
                List.of(new A2aModels.Artifact(
                        List.of(A2aModels.Part.text(result.path("text").asText())),
                        "response", false, true)));
    }

    /** 按设备签发不可猜测的一次性设备 token：32 字节随机数，hex 64 字符。 */
    private String newToken() {
        byte[] bytes = new byte[TOKEN_BYTES];
        random.nextBytes(bytes);
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(Character.forDigit((b >> 4) & 0xf, 16));
            sb.append(Character.forDigit(b & 0xf, 16));
        }
        return sb.toString();
    }

    private class Session {
        final String id;
        final WebSocketSession socket;
        final Map<String, CompletableFuture<JsonNode>> pending = new ConcurrentHashMap<>();
        volatile boolean busy;
        volatile long lastReceive = System.nanoTime();

        Session(String id, WebSocketSession socket) {
            this.id = id;
            this.socket = socket;
        }

        void write(JsonNode node) throws IOException {
            String body = json.writeValueAsString(node);
            if (body.getBytes(StandardCharsets.UTF_8).length > 8192) {
                throw new IOException("Message exceeds 8192 bytes");
            }
            synchronized (socket) {
                socket.sendMessage(new TextMessage(body));
            }
        }
    }

    @PreDestroy
    public void stop() {
        connections.values().forEach(this::closeSocket);
        timer.shutdownNow();
    }
}
