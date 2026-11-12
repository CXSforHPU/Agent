package com.agentconsole.conversation;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;

/**
 * 会话。device_id 为空表示这是网站侧 agent 的会话。
 *
 * @param id        主键
 * @param userId    归属用户
 * @param deviceId  关联设备，网站侧会话为 null
 * @param agentKind website 或 edge
 * @param title     会话标题，通常取首条用户消息的前若干字
 * @param createdAt 创建时间
 * @param updatedAt 最近一次追加消息的时间
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public record Conversation(long id, long userId, Long deviceId, String agentKind,
                           String title, String createdAt, String updatedAt) {
}
