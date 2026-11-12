package com.agentconsole.device;

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
            "id, user_id, name, address, group_name, note, enabled, created_at, last_seen_at, online";

    private final JdbcTemplate jdbc;

    public DeviceRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    private Device map(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        return new Device(rs.getLong("id"), rs.getLong("user_id"), rs.getString("name"),
                rs.getString("address"), rs.getString("group_name"), rs.getString("note"),
                rs.getInt("enabled") == 1, rs.getString("created_at"),
                rs.getString("last_seen_at"), rs.getInt("online") == 1);
    }

    public List<Device> findByUser(long userId) {
        return jdbc.query("SELECT " + COLUMNS + " FROM device WHERE user_id = ? ORDER BY group_name, name",
                this::map, userId);
    }

    public Optional<Device> findById(long id, long userId) {
        var rows = jdbc.query("SELECT " + COLUMNS + " FROM device WHERE id = ? AND user_id = ?",
                this::map, id, userId);
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

    public void updateProbeResult(long id, boolean online) {
        if (online) {
            jdbc.update("UPDATE device SET online = 1, last_seen_at = datetime('now','localtime') WHERE id = ?", id);
        } else {
            jdbc.update("UPDATE device SET online = 0 WHERE id = ?", id);
        }
    }

    /** 只取启用的设备，探活轮询用。 */
    public List<Device> findEnabled() {
        return jdbc.query("SELECT " + COLUMNS + " FROM device WHERE enabled = 1", this::map);
    }
}
