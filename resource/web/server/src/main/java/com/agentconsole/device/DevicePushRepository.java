package com.agentconsole.device;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

import java.security.SecureRandom;
import java.util.Optional;

@Repository
public class DevicePushRepository {

    private static final SecureRandom RANDOM = new SecureRandom();
    private static final String COLUMNS = "device_id, token, url, pushed_at";

    private final JdbcTemplate jdbc;

    public DevicePushRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    private DevicePushConfig map(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        return new DevicePushConfig(rs.getLong("device_id"), rs.getString("token"),
                rs.getString("url"), rs.getLong("pushed_at"));
    }

    public Optional<DevicePushConfig> findByDevice(long deviceId) {
        return jdbc.query("SELECT " + COLUMNS + " FROM device_push WHERE device_id = ?",
                this::map, deviceId).stream().findFirst();
    }

    public Optional<DevicePushConfig> findByToken(String token) {
        return jdbc.query("SELECT " + COLUMNS + " FROM device_push WHERE token = ?",
                this::map, token).stream().findFirst();
    }

    /**
     * 取出这台设备的回调配置，没有就建一条带新 token 的。
     *
     * <p>token 只在第一次建行时生成，之后一直用同一个：板端把地址存进 SD 卡，
     * 换了 token 板端就认不上了。
     */
    public DevicePushConfig ensure(long deviceId) {
        Optional<DevicePushConfig> found = findByDevice(deviceId);
        if (found.isPresent()) {
            return found.get();
        }
        jdbc.update("INSERT OR IGNORE INTO device_push (device_id, token) VALUES (?, ?)",
                deviceId, newToken());
        return findByDevice(deviceId).orElseThrow();
    }

    /** 记下已经下发给板端的回调地址。 */
    public void markPushed(long deviceId, String url) {
        jdbc.update("UPDATE device_push SET url = ?, pushed_at = ? WHERE device_id = ?",
                url, System.currentTimeMillis(), deviceId);
    }

    private static String newToken() {
        byte[] bytes = new byte[16];
        RANDOM.nextBytes(bytes);
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(Character.forDigit((b >> 4) & 0xf, 16));
            sb.append(Character.forDigit(b & 0xf, 16));
        }
        return sb.toString();
    }
}
