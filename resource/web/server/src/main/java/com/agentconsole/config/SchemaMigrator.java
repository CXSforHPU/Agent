package com.agentconsole.config;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import javax.sql.DataSource;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

/**
 * 老 SQLite 库的增量迁移。
 *
 * <p>schema.sql 每次启动都跑，但里面的 CREATE TABLE IF NOT EXISTS 对已存在的
 * device 表不会补列，所以新列由这里用 PRAGMA 查过以后逐列 ALTER TABLE ADD COLUMN。
 * 全部幂等：新库建表时列已齐，这里一条都不执行；老库缺哪列补哪列。
 *
 * <p>唯一激活的数据库保证也在这儿建：设备 token 全局唯一、同一地址只允许一行
 * 处于 activated、设备登录用户名全局唯一。SQLite 的部分索引只约束非空/命中的行，
 * 不影响还没激活的设备。
 */
@Component
public class SchemaMigrator {

    private static final Logger log = LoggerFactory.getLogger(SchemaMigrator.class);

    /** device 表需要补齐的新列：列名 -> 定义（含类型与默认值）。 */
    private static final List<String[]> DEVICE_COLUMNS = List.of(
            new String[]{"activation", "TEXT NOT NULL DEFAULT 'unactivated'"},
            new String[]{"cred_username", "TEXT"},
            new String[]{"cred_salt", "TEXT"},
            new String[]{"cred_hash", "TEXT"},
            new String[]{"device_token", "TEXT"},
            new String[]{"activated_at", "TEXT"},
            new String[]{"card_id", "INTEGER"});

    private final DataSource dataSource;

    public SchemaMigrator(DataSource dataSource) {
        this.dataSource = dataSource;
    }

    @jakarta.annotation.PostConstruct
    public void migrate() throws Exception {
        try (var connection = dataSource.getConnection()) {
            if (!tableExists(connection, "device")) {
                // schema.sql 还没跑到（顺序不保证），建表语句里列是全的，这里不用管
                return;
            }
            Set<String> existing = columns(connection, "device");
            List<String> missing = new ArrayList<>();
            for (String[] column : DEVICE_COLUMNS) {
                if (!existing.contains(column[0])) {
                    missing.add("ALTER TABLE device ADD COLUMN " + column[0] + " " + column[1]);
                }
            }
            try (var statement = connection.createStatement()) {
                for (String sql : missing) {
                    statement.execute(sql);
                    log.info("数据库迁移：{}", sql);
                }
                // 唯一激活的兜底约束，幂等
                statement.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_device_token "
                        + "ON device (device_token) WHERE device_token IS NOT NULL");
                statement.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_device_active_addr "
                        + "ON device (address) WHERE activation = 'activated'");
                statement.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_device_cred_username "
                        + "ON device (cred_username) WHERE cred_username IS NOT NULL");
            }
            if (!missing.isEmpty()) {
                log.info("数据库迁移完成，补了 {} 列", missing.size());
            }
        }
    }

    private static boolean tableExists(java.sql.Connection connection, String table)
            throws SQLException {
        try (var ps = connection.prepareStatement(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?")) {
            ps.setString(1, table);
            try (ResultSet rs = ps.executeQuery()) {
                return rs.next();
            }
        }
    }

    private static Set<String> columns(java.sql.Connection connection, String table)
            throws SQLException {
        Set<String> names = new HashSet<>();
        try (var ps = connection.prepareStatement("PRAGMA table_info(" + table + ")");
             ResultSet rs = ps.executeQuery()) {
            while (rs.next()) {
                names.add(rs.getString("name"));
            }
        }
        return names;
    }
}
