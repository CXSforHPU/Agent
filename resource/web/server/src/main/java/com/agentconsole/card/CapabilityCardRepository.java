package com.agentconsole.card;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.List;
import java.util.Optional;

/**
 * 能力卡表访问。skills 以 JSON 文本存储，读写时与 {@link CapabilityCard.Skill} 列表互转。
 */
@Repository
public class CapabilityCardRepository {

    private static final String COLUMNS =
            "id, user_id, name, description, version, skills, created_at, updated_at";

    private final JdbcTemplate jdbc;
    private final ObjectMapper json;

    public CapabilityCardRepository(JdbcTemplate jdbc, ObjectMapper json) {
        this.jdbc = jdbc;
        this.json = json;
    }

    private CapabilityCard map(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        return new CapabilityCard(rs.getLong("id"), rs.getLong("user_id"),
                rs.getString("name"), rs.getString("description"), rs.getString("version"),
                parseSkills(rs.getString("skills")), rs.getString("created_at"),
                rs.getString("updated_at"), 0);
    }

    private List<CapabilityCard.Skill> parseSkills(String raw) {
        if (raw == null || raw.isBlank()) {
            return List.of();
        }
        try {
            return json.readValue(raw, new TypeReference<List<CapabilityCard.Skill>>() {
            });
        } catch (Exception e) {
            return List.of();
        }
    }

    private String writeSkills(List<CapabilityCard.Skill> skills) {
        try {
            return json.writeValueAsString(skills == null ? List.of() : skills);
        } catch (Exception e) {
            return "[]";
        }
    }

    /** 当前账号的全部能力卡，带绑定设备数。 */
    public List<CapabilityCard> findByUser(long userId) {
        return jdbc.query(
                "SELECT " + COLUMNS + ", (SELECT COUNT(*) FROM device d WHERE d.card_id = capability_card.id) AS device_count "
                        + "FROM capability_card WHERE user_id = ? ORDER BY updated_at DESC, id DESC",
                (rs, i) -> {
                    CapabilityCard card = map(rs, i);
                    return card.withDeviceCount(rs.getLong("device_count"));
                },
                userId);
    }

    public Optional<CapabilityCard> findById(long id, long userId) {
        var rows = jdbc.query(
                "SELECT " + COLUMNS + ", (SELECT COUNT(*) FROM device d WHERE d.card_id = capability_card.id) AS device_count "
                        + "FROM capability_card WHERE id = ? AND user_id = ?",
                (rs, i) -> {
                    CapabilityCard card = map(rs, i);
                    return card.withDeviceCount(rs.getLong("device_count"));
                },
                id, userId);
        return rows.stream().findFirst();
    }

    public CapabilityCard create(long userId, String name, String description, String version,
                                 List<CapabilityCard.Skill> skills) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.update(con -> {
            PreparedStatement ps = con.prepareStatement(
                    "INSERT INTO capability_card (user_id, name, description, version, skills) "
                            + "VALUES (?, ?, ?, ?, ?)",
                    Statement.RETURN_GENERATED_KEYS);
            ps.setLong(1, userId);
            ps.setString(2, name);
            ps.setString(3, description == null ? "" : description);
            ps.setString(4, version == null || version.isBlank() ? "1.0.0" : version);
            ps.setString(5, writeSkills(skills));
            return ps;
        }, keys);
        return findById(keys.getKey().longValue(), userId).orElseThrow();
    }

    public boolean update(long id, long userId, String name, String description, String version,
                          List<CapabilityCard.Skill> skills) {
        return jdbc.update(
                "UPDATE capability_card SET name = ?, description = ?, version = ?, skills = ?, "
                        + "updated_at = datetime('now','localtime') WHERE id = ? AND user_id = ?",
                name, description == null ? "" : description,
                version == null || version.isBlank() ? "1.0.0" : version,
                writeSkills(skills), id, userId) > 0;
    }

    /**
     * 删除能力卡。先把绑着它的设备置空再删卡，与外键 ON DELETE SET NULL 行为一致，
     * 不会留下指向已删卡的悬挂 card_id。
     */
    public boolean delete(long id, long userId) {
        Integer bound = jdbc.queryForObject(
                "SELECT COUNT(*) FROM device WHERE card_id = ? AND user_id = ?", Integer.class,
                id, userId);
        if (bound != null && bound > 0) {
            jdbc.update("UPDATE device SET card_id = NULL WHERE card_id = ? AND user_id = ?",
                    id, userId);
        }
        return jdbc.update("DELETE FROM capability_card WHERE id = ? AND user_id = ?",
                id, userId) > 0;
    }
}
