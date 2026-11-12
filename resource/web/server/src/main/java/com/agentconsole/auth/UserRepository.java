package com.agentconsole.auth;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.Optional;

@Repository
public class UserRepository {

    private final JdbcTemplate jdbc;

    public UserRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    public Optional<User> findByUsername(String username) {
        var rows = jdbc.query(
                "SELECT id, username, password_hash, salt FROM app_user WHERE username = ?",
                (rs, i) -> new User(rs.getLong("id"), rs.getString("username"),
                        rs.getString("password_hash"), rs.getString("salt")),
                username);
        return rows.stream().findFirst();
    }

    public User create(String username, String passwordHash, String salt) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.update(con -> {
            PreparedStatement ps = con.prepareStatement(
                    "INSERT INTO app_user (username, password_hash, salt) VALUES (?, ?, ?)",
                    Statement.RETURN_GENERATED_KEYS);
            ps.setString(1, username);
            ps.setString(2, passwordHash);
            ps.setString(3, salt);
            return ps;
        }, keys);
        long id = keys.getKey().longValue();
        return new User(id, username, passwordHash, salt);
    }
}
