#include "skill.h"
#include <string.h>

extern const struct agent_skill agent_skill_table[];
extern const size_t agent_skill_table_count;
extern const char agent_skill_catalog[];

size_t agent_skill_count(void)
{
    return agent_skill_table_count;
}

const struct agent_skill *agent_skill_get(size_t index)
{
    if (index >= agent_skill_table_count)
    {
        return NULL;
    }

    return &agent_skill_table[index];
}

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

const char *agent_skill_list(void)
{
    return agent_skill_catalog;
}
