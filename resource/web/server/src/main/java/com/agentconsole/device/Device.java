package com.agentconsole.device;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;

/**
 * 设备表记录。
 *
 * @param id          主键
 * @param userId      归属用户
 * @param name        显示名
 * @param address     端侧 agent 的逻辑地址 ws://设备ID，不是网络 URL
 * @param groupName   分组名，空串表示未分组
 * @param note        备注
 * @param enabled     是否启用，停用的设备不参与探活
 * @param createdAt   创建时间
 * @param lastSeenAt  最近一次探活成功的时间
 * @param online      最近一次探活/实时连接是否在线
 * @param activation  激活状态：unactivated 未激活 / activating 激活中 / activated 已激活
 * @param status      展示状态：unactivated / activating / online / offline，读取时按实时连接算
 * @param cardId      绑定的能力卡 id，未绑定为 null
 * @param cardName    绑定的能力卡名，未绑定为 null
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public record Device(long id, long userId, String name, String address, String groupName,
                     String note, boolean enabled, String createdAt, String lastSeenAt,
                     boolean online, String activation, String status,
                     Long cardId, String cardName) {

    /**
     * 地址归一化：补上协议头，去掉结尾斜杠。
     *
     * <p>同一个板子可能被几个账号各登记一次，地址写法可能不完全一样，
     * 判断是不是同一台板子时都按这个结果比。
     */
    public static String normalizeAddress(String address) {
        String s = address == null ? "" : address.trim();
        com.agentconsole.a2a.WebSocketDeviceService.deviceId(s);
        return s;
    }

    /** 换一个实时状态（激活态与绑定卡不变）。 */
    public Device withStatus(String newStatus) {
        return new Device(id, userId, name, address, groupName, note, enabled, createdAt,
                lastSeenAt, "online".equals(newStatus), activation, newStatus, cardId, cardName);
    }
}
