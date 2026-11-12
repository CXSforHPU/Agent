package com.agentconsole.auth;

import io.jsonwebtoken.Jwts;
import io.jsonwebtoken.security.Keys;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import javax.crypto.SecretKey;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Date;

/**
 * JWT 签发与校验。HS256，密钥从 application.yml 的 agent.jwt.secret 取。
 */
@Service
public class JwtService {

    private final SecretKey key;
    private final Duration ttl;

    public JwtService(@Value("${agent.jwt.secret}") String secret,
                      @Value("${agent.jwt.ttl-hours}") long ttlHours) {
        if (secret == null || secret.length() < 32) {
            throw new IllegalStateException(
                    "agent.jwt.secret 至少 32 字符，生产环境请用环境变量 AGENT_JWT_SECRET 覆盖");
        }
        this.key = Keys.hmacShaKeyFor(secret.getBytes(StandardCharsets.UTF_8));
        this.ttl = Duration.ofHours(ttlHours);
    }

    public String issue(long userId, String username) {
        Instant now = Instant.now();
        return Jwts.builder()
                .subject(String.valueOf(userId))
                .claim("username", username)
                .issuedAt(Date.from(now))
                .expiration(Date.from(now.plus(ttl)))
                .signWith(key)
                .compact();
    }

    /**
     * 解析并校验令牌。
     *
     * @param token 请求头里的 Bearer 令牌
     * @return 载荷，过期或签名不对抛异常
     */
    public Claims parse(String token) {
        var payload = Jwts.parser().verifyWith(key).build()
                .parseSignedClaims(token)
                .getPayload();
        return new Claims(Long.parseLong(payload.getSubject()), payload.get("username", String.class));
    }

    public record Claims(long userId, String username) {
    }
}
