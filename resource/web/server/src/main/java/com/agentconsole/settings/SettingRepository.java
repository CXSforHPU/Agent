package com.agentconsole.settings;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

import java.util.HashMap;
import java.util.Map;

@Repository
public class SettingRepository {

    private final JdbcTemplate jdbc;

    public SettingRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    /** 读某个用户的全部设置。 */
    public Map<String, String> findAll(long userId) {
        Map<String, String> out = new HashMap<>();
        jdbc.query("SELECT key, value FROM user_setting WHERE user_id = ?",
                rs -> {
                    out.put(rs.getString("key"), rs.getString("value"));
                },
                userId);
        return out;
    }

    /** 写一条，key 已存在就覆盖。 */
    public void put(long userId, String key, String value) {
        jdbc.update("INSERT INTO user_setting (user_id, key, value) VALUES (?, ?, ?) "
                        + "ON CONFLICT(user_id, key) DO UPDATE SET value = excluded.value, "
                        + "updated_at = datetime('now','localtime')",
                userId, key, value);
    }

    /** 删一条。 */
    public void delete(long userId, String key) {
        jdbc.update("DELETE FROM user_setting WHERE user_id = ? AND key = ?", userId, key);
    }
}
