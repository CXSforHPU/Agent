#ifndef __AGENT_LVGL_CHANNEL_H__
#define __AGENT_LVGL_CHANNEL_H__

#include "AgentChannel.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LVGL 图形通道：聊天界面作为 agent 的输入/输出通道。
 * ops 实例（agent_lvgl_ops）由应用层（lv_home_agent.c）实现并提供；
 * init/reset 只保存/清除 MessageHub 句柄，收发均在 LVGL 线程内完成。
 */
extern AgentChannelOps agent_lvgl_ops;

int agent_lvgl_channel(MessageHub_t hub, Context_t ctx);
void agent_lvgl_channel_reset(void);

/*
 * @brief 获取当前 MessageHub 句柄（供 LVGL UI 发送输入/轮询输出）
 * @return 句柄；agent 未运行或通道未 init 时返回 RT_NULL
 */
MessageHub_t agent_lvgl_channel_hub(void);

#ifdef __cplusplus
}
#endif

#endif /* __AGENT_LVGL_CHANNEL_H__ */
