#ifndef __AGENT_MEMORY_H__
#define __AGENT_MEMORY_H__

/*
 * 分层记忆：working context 之外的三层记忆。
 *
 *   core   : 用户偏好、设备身份、固定约束，每轮都注入
 *   recall : 任务结论、历史事实，按关键词命中注入
 *
 * 记忆是定长槽位的 RAM 池，不申请堆；关掉 PKG_AGENT_MEMORY_ENABLE
 * 时整个模块编成空，调用点拿到的是内联空实现。
 */

#include <rtthread.h>

#ifdef PKG_AGENT_MEMORY_ENABLE

#ifndef PKG_AGENT_MEMORY_CORE_ITEMS
#define PKG_AGENT_MEMORY_CORE_ITEMS 8
#endif

#ifndef PKG_AGENT_MEMORY_RECALL_ITEMS
#define PKG_AGENT_MEMORY_RECALL_ITEMS 32
#endif

#ifndef PKG_AGENT_MEMORY_ITEM_BYTES
#define PKG_AGENT_MEMORY_ITEM_BYTES 192
#endif

#ifndef PKG_AGENT_MEMORY_TOPK
#define PKG_AGENT_MEMORY_TOPK 3
#endif

#ifndef PKG_AGENT_MEMORY_INJECT_BYTES
#define PKG_AGENT_MEMORY_INJECT_BYTES 1024
#endif

typedef enum agent_memory_kind
{
    AGENT_MEMORY_CORE = 0,   /* 常驻：用户偏好、设备身份、固定约束 */
    AGENT_MEMORY_RECALL = 1, /* 检索命中才注入：任务结论、历史事实 */
    AGENT_MEMORY_KIND_MAX
} agent_memory_kind_t;

/*
 * @brief 初始化记忆池：清空 RAM，启用持久化时从文件恢复
 * @return RT_EOK 成功
 */
rt_err_t agent_memory_init(void);

/*
 * @brief 释放记忆：启用持久化时把脏数据写回文件
 */
void agent_memory_deinit(void);

/*
 * @brief 写入一条记忆，内容相同则不重复存
 * @param kind 记忆层
 * @param text 文本，换行会被压成空格，超长截断
 * @return RT_EOK 成功，-RT_EINVAL 参数非法
 */
rt_err_t agent_memory_store(agent_memory_kind_t kind, const char *text);

/*
 * @brief 按编号删除一条记忆
 * @param id 记忆编号
 * @return RT_EOK 成功，-RT_ENOENT 没有这条
 */
rt_err_t agent_memory_delete(int id);

/*
 * @brief 某一层当前条数
 * @param kind 记忆层
 * @return 条数
 */
int agent_memory_count(agent_memory_kind_t kind);

/*
 * @brief 列出某一层全部记忆，新的在前
 * @param kind 记忆层
 * @param buf  输出缓冲
 * @param cap  缓冲容量
 * @return 写出的条数
 */
int agent_memory_list(agent_memory_kind_t kind, char *buf, rt_size_t cap);

/*
 * @brief 按关键词检索
 * @param kind  记忆层，传 -1 表示 core 与 recall 都查
 * @param query 查询文本，空串时只返回 core
 * @param limit 最多返回条数，0 用 PKG_AGENT_MEMORY_TOPK
 * @param buf   输出缓冲，每行 "[id=N kind] text"
 * @param cap   缓冲容量
 * @return 写出的条数
 */
int agent_memory_query(int kind, const char *query, int limit, char *buf, rt_size_t cap);

/*
 * @brief 拼这一轮要注入模型的记忆文本
 * @param query 当前用户请求，空指针时只注入 core
 * @return 静态缓冲里的文本；没有可注入内容时返回 RT_NULL
 */
const char *agent_memory_context(const char *query);

/*
 * @brief 记忆层名称，"core" 或 "recall"
 */
const char *agent_memory_kind_name(agent_memory_kind_t kind);

/*
 * @brief 解析记忆层名称，失败返回 -1
 */
int agent_memory_kind_parse(const char *name);

/*
 * @brief 取某一层里第 index 条（0 为最新）的文本，供测试与调试
 * @param kind  记忆层
 * @param index 序号
 * @param id    输出编号，可为 NULL
 * @return 文本指针，越界返回 RT_NULL
 */
const char *agent_memory_item_text(agent_memory_kind_t kind, int index, int *id);

#else /* !PKG_AGENT_MEMORY_ENABLE */

static inline rt_err_t agent_memory_init(void) { return RT_EOK; }
static inline void agent_memory_deinit(void) { }
static inline rt_err_t agent_memory_store(int kind, const char *text)
{
    (void)kind;
    (void)text;
    return -RT_ENOSYS;
}
static inline rt_err_t agent_memory_delete(int id)
{
    (void)id;
    return -RT_ENOSYS;
}
static inline int agent_memory_count(int kind)
{
    (void)kind;
    return 0;
}
static inline int agent_memory_list(int kind, char *buf, rt_size_t cap)
{
    (void)kind;
    (void)buf;
    (void)cap;
    return 0;
}
static inline int agent_memory_query(int kind, const char *query, int limit,
                                     char *buf, rt_size_t cap)
{
    (void)kind;
    (void)query;
    (void)limit;
    (void)buf;
    (void)cap;
    return 0;
}
static inline const char *agent_memory_context(const char *query)
{
    (void)query;
    return RT_NULL;
}
static inline const char *agent_memory_kind_name(int kind)
{
    (void)kind;
    return "";
}
static inline int agent_memory_kind_parse(const char *name)
{
    (void)name;
    return -1;
}
static inline const char *agent_memory_item_text(int kind, int index, int *id)
{
    (void)kind;
    (void)index;
    if (id) *id = 0;
    return RT_NULL;
}

#endif /* PKG_AGENT_MEMORY_ENABLE */

#endif /* __AGENT_MEMORY_H__ */
