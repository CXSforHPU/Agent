#ifndef __AGENT_TOOL_BASE_H__
#define __AGENT_TOOL_BASE_H__

#include <rtthread.h>
#include "cJSON.h"
#include "utils.h"
#include "chat.h"

/* 工具返回值结构 */
typedef struct AgentToolRet
{
    rt_err_t ret;
    Messages_t messages;
} AgentToolRet, *AgentToolRet_t;

/* 工具链表节点 */
typedef struct AgentToolList
{
    cJSON *tool_obj;
    AgentToolRet ret;
    void (*execute_func)(cJSON *param, struct AgentToolList *node);
    struct AgentToolList *next;
} AgentToolList, AgentToolNode, *AgentToolNode_t;

/*
 * @brief 创建工具链表头节点
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t agent_tool_list_create(void);

/*
 * @brief 追加工具到链表
 * @param tool_obj      工具 JSON 对象
 * @param execute_func  工具执行函数
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t append_tool(cJSON *tool_obj, void (*execute_func)(cJSON *param, struct AgentToolList *node));

/*
 * @brief 构建所有工具的 JSON 数组
 * @return 工具 JSON 数组指针
 */
cJSON *build_tools_json(void);

/*
 * @brief 释放工具节点（仅释放节点自身与 ret.messages）
 * @note tool_obj 的所有权在 build_tools_json 时已移交给 AgentTools 数组，
 *       由 agent_tool_list_destroy 统一释放，此处不得再 cJSON_Delete(tool_obj)
 * @param node 要释放的节点
 */
void free_agent_tool_node(AgentToolNode_t node);

/*
 * @brief 销毁整个工具链表（含 AgentTools 数组及各节点残留的 ret.messages）
 * @note 工具链为全局单例，仅在 agent 清理（cleanup_agent）时调用；
 *       调用后需复位 tool_func 的注册标志，再次进入时重新注册
 */
void agent_tool_list_destroy(void);

/*
 * @brief 根据 tool_obj 查找匹配的工具节点
 * @param tool_obj 工具 JSON 对象
 * @return 找到返回节点指针，未找到返回 NULL
 */
AgentToolNode_t search_agent_tool_node(cJSON *tool_obj);

/*
 * @brief 获取已构建的工具 JSON 数组
 * @return 工具 JSON 数组指针
 */
cJSON *get_agent_tools(void);

#endif /* __AGENT_TOOL_BASE_H__ */