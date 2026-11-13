package com.agentconsole.device;

/**
 * 一台设备的上报回调配置。
 *
 * @param deviceId 设备 id
 * @param token    回调地址里的随机串，板端拿它来上报
 * @param url      已经下发给板端的回调地址，空串表示还没下发过
 * @param pushedAt 下发时刻的毫秒时间戳，0 表示没下发过
 */
public record DevicePushConfig(long deviceId, String token, String url, long pushedAt) {
}
