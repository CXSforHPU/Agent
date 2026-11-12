package com.agentconsole.auth;

import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import java.security.SecureRandom;
import java.util.HexFormat;

/**
 * PBKDF2 口令摘要。每用户独立盐，迭代 120000 次。
 * 用 JDK 自带实现，不额外引依赖。
 */
public final class PasswordHasher {

    private static final int ITERATIONS = 120_000;
    private static final int KEY_LENGTH = 256;
    private static final int SALT_BYTES = 16;
    private static final SecureRandom RANDOM = new SecureRandom();

    private PasswordHasher() {
    }

    public static String newSalt() {
        byte[] salt = new byte[SALT_BYTES];
        RANDOM.nextBytes(salt);
        return HexFormat.of().formatHex(salt);
    }

    public static String hash(String rawPassword, String saltHex) {
        try {
            byte[] salt = HexFormat.of().parseHex(saltHex);
            PBEKeySpec spec = new PBEKeySpec(rawPassword.toCharArray(), salt, ITERATIONS, KEY_LENGTH);
            byte[] digest = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
                    .generateSecret(spec)
                    .getEncoded();
            return HexFormat.of().formatHex(digest);
        } catch (Exception e) {
            throw new IllegalStateException("口令摘要计算失败", e);
        }
    }

    public static boolean matches(String rawPassword, String saltHex, String expectedHashHex) {
        return java.security.MessageDigest.isEqual(
                hash(rawPassword, saltHex).getBytes(),
                expectedHashHex.getBytes());
    }
}
