package com.agentconsole.auth;

/**
 * 用户表记录。
 *
 * @param id           主键
 * @param username     登录名
 * @param passwordHash PBKDF2 摘要，十六进制
 * @param salt         每用户独立盐，十六进制
 */
public record User(long id, String username, String passwordHash, String salt) {
}
