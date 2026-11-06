#include "tool_skill.h"
#include "skill.h"

void tool_skill(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *name;
    const struct agent_skill *skill;
    const char *content;
    rt_err_t result = RT_EOK;

    if (node == RT_NULL)
    {
        return;
    }

    messages_destroy(node->ret.messages);
    node->ret.messages = RT_NULL;
    node->ret.ret = -RT_ERROR;

    name = cJSON_GetObjectItemCaseSensitive(args_obj, "name");
    if (!cJSON_IsObject(args_obj) || (name && !cJSON_IsString(name)))
    {
        content = "Invalid skill arguments: expected an object with an optional string name.";
        result = -RT_EINVAL;
    }
    else if (name == RT_NULL)
    {
        content = agent_skill_list();
    }
    else
    {
        skill = agent_skill_find(name->valuestring);
        if (skill == RT_NULL)
        {
            content = "Skill not found. Call skill without name to list available skills.";
            result = -RT_EINVAL;
        }
        else
        {
            content = skill->content;
        }
    }

    node->ret.messages = messages_create(1);
    if (node->ret.messages == RT_NULL)
    {
        node->ret.ret = -RT_ENOMEM;
        return;
    }

    if (messages_append(node->ret.messages, TYPE_TEXT, content) != RT_EOK)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
        node->ret.ret = -RT_ENOMEM;
        return;
    }

    node->ret.ret = result;
}

static void skill_desc(cJSON *func)
{
    tool_desc(func, "Discover and load built-in task instructions.");
    tool_desc_more(func, "Call without name to list skill names and titles, then call with a name "
        "to read its full Markdown instructions.");
    tool_desc_more(func, "Check for a relevant skill before solving a task and follow its "
        "instructions.");
    tool_desc_more(func, "Loading a skill only reads instructions; it does not execute actions.");
}

static void skill_params(cJSON *props, cJSON *required)
{
    create_property(props, "name", "string",
        "Exact skill name from the catalog; omit to list available skills.");
}

static const struct agent_tool_def skill_tools[] =
{
    {
        "skill",
        skill_desc,
        skill_params,
        tool_skill
    }
};

AGENT_TOOL_MODULE_EXPORT(skill, skill_tools)
