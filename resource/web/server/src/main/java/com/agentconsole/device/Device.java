package com.agentconsole.device;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;

/**
 * 设备表记录。
 *
 * @param id          主键
 * @param userId      归属用户
 * @param name        显示名
 * @param address     端侧 agent 的服务根地址，能力卡在 address/.well-known/agent.json
 * @param groupName   分组名，空串表示未分组
 * @param note        备注
 * @param enabled     是否启用，停用的设备不参与探活
 * @param createdAt   创建时间
 * @param lastSeenAt  最近一次探活成功的时间
 * @param online      最近一次探活是否成功
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public record Device(long id, long userId, String name, String address, String groupName,
                     String note, boolean enabled, String createdAt, String lastSeenAt,
                     boolean online) {
}
