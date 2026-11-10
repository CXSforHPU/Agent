/*
 * 内置技能表：按序号或名字查询技能条目，以及技能目录文本
 * 技能表与目录文本是编译期常量，本文件只提供只读访问。
 */
#include "skill.h"
#include <string.h>

extern const struct agent_skill agent_skill_table[];
extern const size_t agent_skill_table_count;
extern const char agent_skill_catalog[];

/*
 * @brief 取内置技能总数
 * @return 技能条目数
 */
size_t agent_skill_count(void)
{
    return agent_skill_table_count;
}

/*
 * @brief 按序号取技能条目
 * @param index 技能序号
 * @return 技能条目指针，越界返回 NULL
 */
const struct agent_skill *agent_skill_get(size_t index)
{
    if (index >= agent_skill_table_count)
    {
        return NULL;
    }

    return &agent_skill_table[index];
}

/*
 * @brief 按名字查找技能条目
 * @param name 技能名
 * @return 技能条目指针，未找到返回 NULL
 */
const struct agent_skill *agent_skill_find(const char *name)
{
    size_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0; i < agent_skill_table_count; i++)
    {
        if (strcmp(name, agent_skill_table[i].name) == 0)
        {
            return &agent_skill_table[i];
        }
    }

    return NULL;
}

/*
 * @brief 取技能目录文本
 * @return 目录文本（编译期常量，调用方不得释放）
 */
const char *agent_skill_list(void)
{
    return agent_skill_catalog;
}
