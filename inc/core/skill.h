#ifndef __AGENT_SKILL_H__
#define __AGENT_SKILL_H__

#include <stddef.h>

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

#endif
