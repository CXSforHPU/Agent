package com.agentconsole.device;

import com.agentconsole.a2a.A2aClient;
import com.agentconsole.a2a.A2aModels;
import com.agentconsole.auth.PasswordHasher;
import com.agentconsole.ops.OpsService;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 设备业务：登记、编辑、探活、激活凭据下发与实时状态。
 *
 * <p>状态口径（读取时算，Web 刷新即时）：
 * <ul>
 *   <li>未激活 unactivated：设备行建好但没配激活凭据；</li>
 *   <li>激活中 activating：账号密码配好，等板端来连；</li>
 *   <li>在线 online：已激活且当前有实时 WebSocket 会话；</li>
 *   <li>未在线 offline：已激活但当前没连接。</li>
 * </ul>
 */
@Service
public class DeviceService {

    private static final Logger log = LoggerFactory.getLogger(DeviceService.class);

    private final DeviceRepository repo;
    private final A2aClient a2a;
    private final OpsService ops;
    private final ObjectMapper json;

    public DeviceService(DeviceRepository repo, A2aClient a2a,
                         OpsService ops, ObjectMapper json) {
        this.repo = repo;
        this.a2a = a2a;
        this.ops = ops;
        this.json = json;
    }

    public List<Device> list(long userId) {
        return repo.findByUser(userId).stream().map(this::withStatus).toList();
    }

    public Device get(long id, long userId) {
        return withStatus(repo.findById(id, userId)
                .orElseThrow(() -> new IllegalArgumentException("设备不存在")));
    }

    /**
     * 登记设备。地址只能是 ws://设备ID（逻辑标识，服务端校验，不能任造网络 URL）。
     *
     * @param username 设备登录用户名，可空；给了就同时要密码
     * @param password 设备登录密码，只存 PBKDF2 摘要 + 盐，明文不落库不回传
     */
    public Device create(long userId, String name, String address, String groupName, String note,
                         String username, String password) {
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("设备名不能为空");
        }
        if (address == null || address.isBlank()) {
            throw new IllegalArgumentException("地址不能为空");
        }
        boolean noUser = username == null || username.isBlank();
        boolean noPass = password == null || password.isBlank();
        if (noUser != noPass) {
            throw new IllegalArgumentException("设备账号与密码必须同时填写");
        }
        Device created = repo.create(userId, name.trim(), normalize(address), groupName, note);
        if (!noUser) {
            setCredential(created.id(), username.trim(), password);
        }
        return get(created.id(), userId);
    }

    /**
     * 下发/更换设备激活凭据。设备一旦已激活就拒绝换凭据（禁止换密码换 token）。
     * 只有摘要与盐入库，明文仅在调用链里短暂存在，不打印不落日志。
     */
    public Device setCredential(long deviceId, String username, String password) {
        if (username == null || username.isBlank()) {
            throw new IllegalArgumentException("设备账号不能为空");
        }
        if (password == null || password.length() < 6) {
            throw new IllegalArgumentException("设备密码至少 6 位");
        }
        if (repo.isActivated(deviceId)) {
            throw new IllegalStateException("设备已激活，不能更换激活凭据；如需重新激活请先删除设备");
        }
        String salt = PasswordHasher.newSalt();
        String hash = PasswordHasher.hash(password, salt);
        try {
            if (!repo.setCredential(deviceId, username, salt, hash)) {
                throw new IllegalStateException(
                        "设备已激活，不能更换激活凭据；如需重新激活请先删除设备");
            }
        } catch (org.springframework.dao.DuplicateKeyException e) {
            // idx_device_cred_username 唯一索引：同一账号已被别的设备占用
            throw new IllegalArgumentException("设备账号已被其他设备使用，请换一个账号");
        } catch (org.springframework.dao.DataAccessException e) {
            String detail = e.getMessage() == null ? "" : e.getMessage();
            if (detail.contains("UNIQUE")) {
                throw new IllegalArgumentException("设备账号已被其他设备使用，请换一个账号");
            }
            throw e;
        }
        return get(deviceId, repo.findAll().stream()
                .filter(d -> d.id() == deviceId).findFirst().map(Device::userId).orElse(0L));
    }

    public Device update(long id, long userId, String name, String address,
                         String groupName, String note, boolean enabled) {
        Device before = get(id, userId);
        String normalized = normalize(address);
        // 已激活设备改地址等于换设备身份，旧 token 与新地址对不上，直接拦下
        if ("activated".equals(before.activation()) && !before.address().equals(normalized)) {
            throw new IllegalStateException("设备已激活，不能修改设备 ID；如需换 ID 请先删除设备再登记");
        }
        if (!repo.update(id, userId, name, normalized, groupName, note, enabled)) {
            throw new IllegalArgumentException("设备不存在");
        }
        return get(id, userId);
    }

    public void delete(long id, long userId) {
        if (!repo.delete(id, userId)) {
            throw new IllegalArgumentException("设备不存在");
        }
        // 删除即彻底注销：行与 token 一起消失，同 ID 允许重新登记并再次激活
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

    /** 同一块板子可能被几个账号各登记一次，回调归属取最早那行。 */
    public Device ownerOf(Device device) {
        String address = device.address();
        Device owner = device;
        for (Device candidate : repo.findAll()) {
            if (candidate.id() < owner.id()
                    && address.equals(candidate.address())) {
                owner = candidate;
            }
        }
        return owner;
    }

    /** 配置下发：WebSocket 迁移后暂无对应协议，明确返回不支持，不制造假成功。 */
    public String pushConfig(long id, long userId, String apiKey, String modelName, String baseUrl) {
        get(id, userId);
        return "WebSocket 配置下发暂未提供";
    }

    /** 能力卡已改为绑定模式（capability_card 表），原下发接口不再提供。 */
    public String pushCard(long id, long userId, String name, String description, String version) {
        get(id, userId);
        return "能力卡已改为「绑定能力卡」，请在能力卡页面绑定，不再支持直接下发";
    }

    /** 定时探活所有启用的设备。 */
    @Scheduled(fixedDelayString = "${agent.a2a.probe-interval-ms:15000}")
    public void probeAll() {
        /* 同一块板子只探一次：板端单线程，重复探活会把它的连接占满。 */
        Map<String, List<Device>> byAddress = new LinkedHashMap<>();
        for (Device device : repo.findEnabled()) {
            byAddress.computeIfAbsent(device.address(),
                    key -> new ArrayList<>()).add(device);
        }
        for (List<Device> group : byAddress.values()) {
            Device first = group.get(0);
            try {
                a2a.discover(first.address());
                for (Device device : group) {
                    repo.updateProbeResult(device.id(), true);
                }

            } catch (Exception e) {
                for (Device device : group) {
                    repo.updateProbeResult(device.id(), false);
                }
            }
        }
    }

    /**
     * 按实时连接补状态：已激活且当前有会话 → online，已激活没连接 → offline，
     * 有凭据没 token → activating，没凭据 → unactivated。
     */
    private Device withStatus(Device device) {
        String activation = device.activation() == null ? "unactivated" : device.activation();
        String status;
        if ("activated".equals(activation)) {
            status = a2a.isConnected(device.address()) ? "online" : "offline";
        } else if ("activating".equals(activation)) {
            status = "activating";
        } else {
            status = "unactivated";
        }
        return device.withStatus(status);
    }

    public record ProbeResult(boolean online, A2aModels.AgentCard card, String error) {
    }

    private static String normalize(String address) {
        return Device.normalizeAddress(address);
    }
}
