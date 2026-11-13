package com.agentconsole.device;

/**
 * 端侧 agent 自发上报的一条事件。
 *
 * @param id        事件 id
 * @param deviceId  设备 id
 * @param kind      事件类型，report 表示 agent 自己产生的结论
 * @param text      事件正文
 * @param createdAt 落库时间
 */
public record DeviceEvent(long id, long deviceId, String kind, String text, String createdAt) {
}
