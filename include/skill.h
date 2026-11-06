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

size_t agent_skill_count(void);
const struct agent_skill *agent_skill_get(size_t index);
const struct agent_skill *agent_skill_find(const char *name);
const char *agent_skill_list(void);

#ifdef __cplusplus
}
#endif

#endif
