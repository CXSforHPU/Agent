package com.agentconsole.auth;

import org.springframework.dao.DataAccessException;
import org.springframework.stereotype.Service;

@Service
public class AuthService {

    private final UserRepository users;
    private final JwtService jwt;

    public AuthService(UserRepository users, JwtService jwt) {
        this.users = users;
        this.jwt = jwt;
    }

    public record Token(String token, String username, long userId) {
    }

    public Token register(String username, String rawPassword) {
        if (username == null || username.isBlank() || username.length() > 32) {
            throw new IllegalArgumentException("用户名不能为空且不超过 32 字符");
        }
        if (rawPassword == null || rawPassword.length() < 6) {
            throw new IllegalArgumentException("密码至少 6 位");
        }
        String salt = PasswordHasher.newSalt();
        try {
            User user = users.create(username.trim(), PasswordHasher.hash(rawPassword, salt), salt);
            return new Token(jwt.issue(user.id(), user.username()), user.username(), user.id());
        } catch (DataAccessException e) {
            if (isUniqueViolation(e)) {
                throw new IllegalArgumentException("用户名已存在");
            }
            throw e;
        }
    }

    /**
     * sqlite-jdbc 的唯一约束冲突在 Spring 侧包成 UncategorizedSQLException，
     * 拿不到 DuplicateKeyException，只能顺着原因链找 SQLite 的错误码 19。
     */
    private static boolean isUniqueViolation(Throwable e) {
        for (Throwable t = e; t != null; t = t.getCause()) {
            if (t instanceof java.sql.SQLException sql && sql.getErrorCode() == 19) {
                return true;
            }
            String msg = t.getMessage();
            if (msg != null && msg.contains("UNIQUE constraint")) {
                return true;
            }
            if (t.getCause() == t) {
                break;
            }
        }
        return false;
    }

    public Token login(String username, String rawPassword) {
        User user = users.findByUsername(username == null ? "" : username.trim())
                .orElseThrow(() -> new IllegalArgumentException("用户名或密码错误"));
        if (!PasswordHasher.matches(rawPassword, user.salt(), user.passwordHash())) {
            throw new IllegalArgumentException("用户名或密码错误");
        }
        return new Token(jwt.issue(user.id(), user.username()), user.username(), user.id());
    }
}
