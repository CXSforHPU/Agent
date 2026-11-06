#ifndef __AGENT_CHANNEL_H__
#define __AGENT_CHANNEL_H__

#include "rtthread.h"
#include "MessageHub.h"
#include "context.h"

/*
 * @brief 通道操作接口（ops）
 * 由 AgentLoop 持有单一函数指针 g_channel_ops 统一分发 init/reset，
 * 各通道（CLI/WebNet/Debug）在各自 .c 内实现 ops 实例，
 * 宏选择只在 AgentChannels.h 一处发生。
 */
typedef struct AgentChannelOps
{
    /*
     * @brief 通道初始化：绑定 message_hub / context（如 CLI 创建 readline 线程）
     * @param hub 消息中心句柄
     * @param ctx 上下文管理器句柄
     * @return RT_EOK 成功，其他为失败
     */
    int (*init)(MessageHub_t hub, Context_t ctx);

    /*
     * @brief 通道清理（幂等）：停止通道线程、释放句柄、置空缓存指针
     * 保证清理后不再访问即将销毁的 message_hub / context
     */
    void (*reset)(void);
} AgentChannelOps;

#endif /* __AGENT_CHANNEL_H__ */
