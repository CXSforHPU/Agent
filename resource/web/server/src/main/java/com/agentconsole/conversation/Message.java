package com.agentconsole.conversation;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;

/**
 * 一条消息。
 *
 * @param id             主键
 * @param conversationId 所属会话
 * @param role           user / assistant
 * @param content        正文
 * @param createdAt      创建时间
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public record Message(long id, long conversationId, String role, String content, String createdAt) {
}
