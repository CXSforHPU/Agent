package com.agentconsole.ops;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

import java.util.List;
import java.util.Optional;

@Repository
public class OpsSettingRepository {

    private final JdbcTemplate jdbc;

    public OpsSettingRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    /** 读一条。 */
    public Optional<String> find(String key) {
        List<String> rows = jdbc.query("SELECT value FROM ops_setting WHERE key = ?",
                (rs, rowNum) -> rs.getString("value"), key);
        return rows.isEmpty() ? Optional.empty() : Optional.of(rows.get(0));
    }

    /** 写一条，key 已存在就覆盖。 */
    public void put(String key, String value) {
        jdbc.update("INSERT INTO ops_setting (key, value) VALUES (?, ?) "
                        + "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
                        + "updated_at = datetime('now','localtime')",
                key, value);
    }
}
