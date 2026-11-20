#ifndef __AGENT_SKILL_H__
#define __AGENT_SKILL_H__

#include <stddef.h>

/*
 * 旧配置没有后加的 PKG_AGENT_SKILL_EMBED，视为技能模块默认开启；
 * 新配置关闭总开关后 EMBED 仍在，用它区分“显式关闭”和“旧配置”。
 */
#if defined(PKG_AGENT_SKILL_ENABLE) || !defined(PKG_AGENT_SKILL_EMBED)
#define AGENT_SKILL_MODULE_ENABLED 1
#else
#define AGENT_SKILL_MODULE_ENABLED 0
#endif

#if AGENT_SKILL_MODULE_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

struct agent_skill
{
    const char *name;
    const char *content;
    size_t size; /**< UTF-8 bytes, excluding the terminating NUL. */
};

/*
 * @brief  取技能总数
 * @return 技能条数
 */
size_t agent_skill_count(void);

/*
 * @brief  按下标取技能
 * @param  index 技能下标
 * @return 技能结构体指针，下标越界返回 NULL
 */
const struct agent_skill *agent_skill_get(size_t index);

/*
 * @brief  按名字查技能
 * @param  name 技能名
 * @return 技能结构体指针，找不到返回 NULL
 */
const struct agent_skill *agent_skill_find(const char *name);

/*
 * @brief  取技能清单文本
 * @return 清单字符串，调用者不释放
 */
const char *agent_skill_list(void);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_SKILL_MODULE_ENABLED */

#endif
