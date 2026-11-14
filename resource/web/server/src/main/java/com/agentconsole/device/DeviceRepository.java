package com.agentconsole.device;

import org.springframework.dao.DataAccessException;
import org.springframework.dao.DuplicateKeyException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.List;
import java.util.Optional;

@Repository
public class DeviceRepository {

    private static final String COLUMNS =
            "d.id, d.user_id, d.name, d.address, d.group_name, d.note, d.enabled, d.created_at, "
                    + "d.last_seen_at, d.online, d.activation, d.card_id, c.name AS card_name";

    private static final String FROM =
            " FROM device d LEFT JOIN capability_card c ON c.id = d.card_id";

    private final JdbcTemplate jdbc;

    public DeviceRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    private Device map(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        long cardId = rs.getLong("card_id");
        boolean cardNull = rs.wasNull();
        String activation = rs.getString("activation");
        return new Device(rs.getLong("id"), rs.getLong("user_id"), rs.getString("name"),
                rs.getString("address"), rs.getString("group_name"), rs.getString("note"),
                rs.getInt("enabled") == 1, rs.getString("created_at"),
                rs.getString("last_seen_at"), rs.getInt("online") == 1,
                activation, activation, cardNull ? null : cardId, rs.getString("card_name"));
    }

    public List<Device> findByUser(long userId) {
        return jdbc.query("SELECT " + COLUMNS + FROM
                        + " WHERE d.user_id = ? ORDER BY d.group_name, d.name",
                this::map, userId);
    }

    /** 所有设备行，不分账号。判断两块板子是不是同一台时用。 */
    public List<Device> findAll() {
        return jdbc.query("SELECT " + COLUMNS + FROM + " ORDER BY d.id", this::map);
    }

    public Optional<Device> findById(long id, long userId) {
        var rows = jdbc.query("SELECT " + COLUMNS + FROM + " WHERE d.id = ? AND d.user_id = ?",
                this::map, id, userId);
        return rows.stream().findFirst();
    }

    /** 按逻辑地址找某账号的设备行。 */
    public Optional<Device> findByAddress(long userId, String address) {
        var rows = jdbc.query("SELECT " + COLUMNS + FROM
                        + " WHERE d.user_id = ? AND d.address = ? ORDER BY d.id",
                this::map, userId, address);
        return rows.stream().findFirst();
    }

    /** 同一地址的全部设备行（同一块板子可能被多个账号登记）。 */
    public List<Device> findByAddress(String address) {
        return jdbc.query("SELECT " + COLUMNS + FROM + " WHERE d.address = ? ORDER BY d.id",
                this::map, address);
    }

    /** 激活流程按登录用户名找行：用户名全局唯一，找到即目标设备。 */
    public Optional<Device> findByCredentialUsername(String username) {
        var rows = jdbc.query("SELECT " + COLUMNS + FROM + " WHERE d.cred_username = ?",
                this::map, username);
        return rows.stream().findFirst();
    }

    public Device create(long userId, String name, String address, String groupName, String note) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.update(con -> {
            PreparedStatement ps = con.prepareStatement(
                    "INSERT INTO device (user_id, name, address, group_name, note) VALUES (?, ?, ?, ?, ?)",
                    Statement.RETURN_GENERATED_KEYS);
            ps.setLong(1, userId);
            ps.setString(2, name);
            ps.setString(3, address);
            ps.setString(4, groupName == null ? "" : groupName);
            ps.setString(5, note == null ? "" : note);
            return ps;
        }, keys);
        return findById(keys.getKey().longValue(), userId).orElseThrow();
    }

    public boolean update(long id, long userId, String name, String address,
                          String groupName, String note, boolean enabled) {
        // group_name 与 note 在表里是 NOT NULL，漏传时要补空串，
        // 否则直接撞约束报 500。
        return jdbc.update(
                "UPDATE device SET name = ?, address = ?, group_name = ?, note = ?, enabled = ? "
                        + "WHERE id = ? AND user_id = ?",
                name, address, groupName == null ? "" : groupName, note == null ? "" : note,
                enabled ? 1 : 0, id, userId) > 0;
    }

    public boolean delete(long id, long userId) {
        return jdbc.update("DELETE FROM device WHERE id = ? AND user_id = ?", id, userId) > 0;
    }

    /**
     * 写入设备登录凭据并把状态置为激活中。
     *
     * <p>只存 PBKDF2 摘要与盐，明文密码不落库。设备一旦已激活（有 token）就拒绝改凭据，
     * 防止换密码换 token 绕过“一台设备只激活一次”。
     *
     * @return false 表示设备不存在或已激活，调用方给冲突提示
     */
    public boolean setCredential(long id, String username, String salt, String hash) {
        return jdbc.update(
                "UPDATE device SET cred_username = ?, cred_salt = ?, cred_hash = ?, "
                        + "activation = 'activating' "
                        + "WHERE id = ? AND activation <> 'activated' AND device_token IS NULL",
                username, salt, hash, id) > 0;
    }

    /**
     * 唯一激活：把一次性设备 token 写进目标行。
     *
     * <p>WHERE 里带上 device_token IS NULL 做条件更新，两个并发激活只有一个能改到行；
     * 再叠加 idx_device_token / idx_device_active_addr 两个唯一索引兜底，
     * 同一 ID 或同一凭据的第二次激活直接抛冲突，绝不发新 token。
     *
     * @return true 表示本次激活成功（自己拿到了 token）
     */
    public boolean activate(long id, String token) {
        try {
            return jdbc.update(
                    "UPDATE device SET device_token = ?, activation = 'activated', "
                            + "activated_at = datetime('now','localtime') "
                            + "WHERE id = ? AND device_token IS NULL",
                    token, id) > 0;
        } catch (DuplicateKeyException e) {
            // 唯一索引拦下了并发的第二次激活
            return false;
        } catch (DataAccessException e) {
            if (e.getMessage() != null && e.getMessage().contains("UNIQUE")) {
                return false;
            }
            throw e;
        }
    }

    /** 已激活的行换 token 一律拒绝（返回 false）。 */
    public boolean isActivated(long id) {
        Integer count = jdbc.queryForObject(
                "SELECT COUNT(*) FROM device WHERE id = ? AND activation = 'activated'",
                Integer.class, id);
        return count != null && count > 0;
    }

    /** 按 token 找已激活设备，WS 重连时校验。 */
    public Optional<Device> findByToken(String token) {
        if (token == null || token.length() < 16) {
            return Optional.empty();
        }
        var rows = jdbc.query("SELECT " + COLUMNS + FROM + " WHERE d.device_token = ?",
                this::map, token);
        return rows.stream().findFirst();
    }

    /** 取设备的激活凭据摘要（校验密码用），没有凭据返回空。 */
    public Optional<Credential> findCredential(long id) {
        var rows = jdbc.query(
                "SELECT cred_username, cred_salt, cred_hash FROM device WHERE id = ?",
                (rs, i) -> rs.getString("cred_username") == null ? null
                        : new Credential(rs.getString("cred_username"), rs.getString("cred_salt"),
                        rs.getString("cred_hash")),
                id);
        return rows.stream().filter(c -> c != null).findFirst();
    }

    public void updateProbeResult(long id, boolean online) {
        if (online) {
            jdbc.update("UPDATE device SET online = 1, last_seen_at = datetime('now','localtime') WHERE id = ?", id);
        } else {
            jdbc.update("UPDATE device SET online = 0 WHERE id = ?", id);
        }
    }

    /** 只取启用的设备，探活轮询用。 */
    public List<Device> findEnabled() {
        return jdbc.query("SELECT " + COLUMNS + FROM + " WHERE d.enabled = 1", this::map);
    }

    /** 绑定能力卡。设备只能有一张卡：单列 card_id 天然保证；卡可被多台设备绑定。 */
    public boolean bindCard(long deviceId, long userId, Long cardId) {
        if (cardId == null) {
            return jdbc.update("UPDATE device SET card_id = NULL WHERE id = ? AND user_id = ?",
                    deviceId, userId) > 0;
        }
        return jdbc.update("UPDATE device SET card_id = ? WHERE id = ? AND user_id = ?",
                cardId, deviceId, userId) > 0;
    }

    /** 设备登录凭据三元组。 */
    public record Credential(String username, String salt, String hash) {
    }
}
