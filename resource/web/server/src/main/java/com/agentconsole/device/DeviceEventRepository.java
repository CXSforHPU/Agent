package com.agentconsole.device;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Optional;

@Repository
public class DeviceEventRepository {

    private static final String COLUMNS = "id, device_id, kind, text, created_at";

    private final JdbcTemplate jdbc;

    public DeviceEventRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    private DeviceEvent map(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        return new DeviceEvent(rs.getLong("id"), rs.getLong("device_id"), rs.getString("kind"),
                rs.getString("text"), rs.getString("created_at"));
    }

    /** 记一条事件，返回带落库时间的完整行。 */
    public DeviceEvent append(long deviceId, String kind, String text) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.update(con -> {
            PreparedStatement ps = con.prepareStatement(
                    "INSERT INTO device_event (device_id, kind, text) VALUES (?, ?, ?)",
                    Statement.RETURN_GENERATED_KEYS);
            ps.setLong(1, deviceId);
            ps.setString(2, kind);
            ps.setString(3, text);
            return ps;
        }, keys);
        return findById(keys.getKey().longValue()).orElseThrow();
    }

    public Optional<DeviceEvent> findById(long id) {
        return jdbc.query("SELECT " + COLUMNS + " FROM device_event WHERE id = ?",
                this::map, id).stream().findFirst();
    }

    /** 取最近的若干条，新的在前。 */
    public List<DeviceEvent> findByDevice(long deviceId, int limit) {
        return jdbc.query("SELECT " + COLUMNS + " FROM device_event WHERE device_id = ? "
                + "ORDER BY id DESC LIMIT ?", this::map, deviceId, limit);
    }

    /**
     * 取多个设备行的最近事件，新的在前。
     *
     * <p>同一块板子被几个账号各登记一次时，事件记在其中任意一行上，
     * 查的时候要把这几行一起查。
     */
    public List<DeviceEvent> findByDevices(List<Long> deviceIds, int limit) {
        if (deviceIds == null || deviceIds.isEmpty()) {
            return List.of();
        }
        String placeholders = String.join(",", Collections.nCopies(deviceIds.size(), "?"));
        List<Object> args = new ArrayList<>(deviceIds);
        args.add(limit);
        return jdbc.query("SELECT " + COLUMNS + " FROM device_event WHERE device_id IN ("
                + placeholders + ") ORDER BY id DESC LIMIT ?", this::map, args.toArray());
    }
}
