/*
 * MQTT 工具 - 本地过滤与阈值规则（咨询识别、越界判定、规则管理）
 * 只把「需要咨询」或「命中阈值规则」的消息交给 agent，例行数据本地拦截。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#include <stdlib.h>

/* ==========================================================================
 * 本地过滤：只把「需要咨询」或「命中阈值规则」的消息交给 agent
 * ========================================================================== */

/* 询问类关键词（小写匹配）：报文里出现即认为需要 agent 咨询/回答 */
static const char *const s_ask_keywords[] =
{
    "?", "help", "how ", "why ", "what ", "should ", "please", "advice",
    "alert", "warning", "error", "fail", "abnormal",
    "咨询", "请问", "怎么", "如何", "为什么", "是否", "求助", "告警", "报警", "异常", "故障", "建议"
};

/*
 * @brief 小写化并判断是否包含子串（ASCII 大小写不敏感，中文按字节原样比较）
 * @param haystack 待查文本
 * @param needle   关键词
 * @return RT_TRUE 命中
 */
static rt_bool_t text_contains_ci(const char *haystack, const char *needle)
{
    size_t nlen;

    if (haystack == RT_NULL || needle == RT_NULL)
    {
        return RT_FALSE;
    }
    nlen = rt_strlen(needle);
    if (nlen == 0)
    {
        return RT_FALSE;
    }

    for (; *haystack != '\0'; haystack++)
    {
        size_t i;
        for (i = 0; i < nlen; i++)
        {
            char a = haystack[i];
            char b = needle[i];
            if (a == '\0')
            {
                return RT_FALSE;
            }
            if (a >= 'A' && a <= 'Z')
            {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b)
            {
                break;
            }
        }
        if (i == nlen)
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

/*
 * @brief 判断报文是否像「需要咨询」
 * @param payload 报文内容
 * @return RT_TRUE 需要交给 agent
 */
rt_bool_t payload_looks_like_ask(const char *payload)
{
    size_t i;

    if (payload == RT_NULL || payload[0] == '\0')
    {
        return RT_FALSE;
    }

    for (i = 0; i < sizeof(s_ask_keywords) / sizeof(s_ask_keywords[0]); i++)
    {
        if (text_contains_ci(payload, s_ask_keywords[i]))
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

/* 前向声明：路径匹配需要用到比较函数（定义在本文件后部） */
static rt_bool_t rule_compare(const char *op, double value, double limit);

/*
 * @brief 把 JSON 节点转成数值（兼容数值字符串与布尔）
 * @param node 节点
 * @param out  输出数值
 * @return RT_TRUE 转换成功
 */
rt_bool_t mqtt_json_node_number(cJSON *node, double *out)
{
    if (node == RT_NULL || out == RT_NULL)
    {
        return RT_FALSE;
    }

    if (cJSON_IsNumber(node))
    {
        *out = node->valuedouble;
        return RT_TRUE;
    }

    /* 设备常把数值上报成字符串，如 {"hum":"35"} */
    if (cJSON_IsString(node) && node->valuestring != RT_NULL && node->valuestring[0] != '\0')
    {
        cJSON *num = cJSON_Parse(node->valuestring);
        rt_bool_t ok = RT_FALSE;
        if (num != RT_NULL)
        {
            if (cJSON_IsNumber(num))
            {
                *out = num->valuedouble;
                ok = RT_TRUE;
            }
            cJSON_Delete(num);
        }
        return ok;
    }

    /* 布尔按 0/1 处理（例如 {"alarm":true}） */
    if (cJSON_IsBool(node))
    {
        *out = node->valueint ? 1.0 : 0.0;
        return RT_TRUE;
    }

    return RT_FALSE;
}

/*
 * @brief 按路径在 JSON 中匹配阈值（支持嵌套对象 / 数组 / 通配）
 *
 * 路径语法：
 *   ""（空）           整条 payload 当数值，如 payload 为 35
 *   "hum"              顶层字段
 *   "sensor.hum"       嵌套字段
 *   "readings[0].t"    数组下标
 *   "readings[*].t"    数组内任一元素命中即算命中
 *   "readings"         字段本身是数组/多值时，同样按"任一元素命中"
 *
 * @param node  当前节点
 * @param path  剩余路径
 * @param op    比较符
 * @param limit 阈值
 * @param hit   命中时的实测值（输出，可为 NULL）
 * @return RT_TRUE 命中
 */
static rt_bool_t json_path_match(cJSON *node, const char *path, const char *op, double limit, double *hit)
{
    char seg[MQTT_TOOL_FIELD_MAX_LEN * 2];
    const char *p = path;
    rt_size_t len = 0;
    cJSON *child = RT_NULL;

    if (node == RT_NULL)
    {
        return RT_FALSE;
    }

    /* 路径走完：当前节点即为候选值，逐个与阈值比较 */
    if (path == RT_NULL || path[0] == '\0')
    {
        double value = 0.0;
        if (mqtt_json_node_number(node, &value) && rule_compare(op, value, limit))
        {
            if (hit != RT_NULL)
            {
                *hit = value;
            }
            return RT_TRUE;
        }
        return RT_FALSE;
    }

    /* 段首是 '['：数组下标或通配，作用在当前数组节点上 */
    if (p[0] == '[')
    {
        rt_bool_t wildcard = (p[1] == '*');
        const char *q = p + 2;              /* 跳过 "[x" 的 '[' 与首字符 */
        const char *rest;
        int index = 0;

        if (!wildcard)
        {
            if (p[1] < '0' || p[1] > '9')
            {
                return RT_FALSE;
            }
            q = p + 1;
            while (*q >= '0' && *q <= '9')
            {
                index = index * 10 + (*q - '0');
                q++;
            }
        }
        if (*q != ']')
        {
            return RT_FALSE;                /* 路径语法错误 */
        }
        q++;
        rest = (*q == '.') ? (q + 1) : q;

        if (!cJSON_IsArray(node))
        {
            return RT_FALSE;
        }

        if (wildcard)
        {
            cJSON_ArrayForEach(child, node)
            {
                if (json_path_match(child, rest, op, limit, hit))
                {
                    return RT_TRUE;
                }
            }
            return RT_FALSE;
        }
        return json_path_match(cJSON_GetArrayItem(node, index), rest, op, limit, hit);
    }

    /* 普通键：读到 '.' 或 '[' 为止 */
    while (*p != '\0' && *p != '.' && *p != '[' && len < sizeof(seg) - 1)
    {
        seg[len++] = *p++;
    }
    if (len == 0)
    {
        return RT_FALSE;
    }
    seg[len] = '\0';
    if (*p == '.')
    {
        p++;
    }

    if (!cJSON_IsObject(node))
    {
        return RT_FALSE;
    }
    child = cJSON_GetObjectItemCaseSensitive(node, seg);
    if (child == RT_NULL)
    {
        return RT_FALSE;
    }

    /* 键取值后路径结束且该值是数组：按"任一元素命中"处理 */
    if (*p == '\0' && cJSON_IsArray(child))
    {
        cJSON_ArrayForEach(node, child)     /* node 复用为迭代变量 */
        {
            if (json_path_match(node, "", op, limit, hit))
            {
                return RT_TRUE;
            }
        }
        return RT_FALSE;
    }

    return json_path_match(child, p, op, limit, hit);
}

/*
 * @brief 判断 payload 是否命中某条规则（含嵌套路径 / 数组 / 通配）
 * @param payload 报文内容
 * @param field   JSON 字段路径；为空则整条 payload 视为数值
 * @param op      比较符
 * @param limit   阈值
 * @param hit     命中时的实测值（输出，可为 NULL）
 * @return RT_TRUE 命中
 */
static rt_bool_t payload_match_rule(const char *payload, const char *field,
                                    const char *op, double limit, double *hit)
{
    cJSON *root;
    rt_bool_t matched = RT_FALSE;

    if (payload == RT_NULL || payload[0] == '\0')
    {
        return RT_FALSE;
    }

    root = cJSON_Parse(payload);
    if (root == RT_NULL)
    {
        return RT_FALSE;
    }

    matched = json_path_match(root, (field != RT_NULL) ? field : "", op, limit, hit);

    cJSON_Delete(root);
    return matched;
}

/*
 * @brief 比较数值与规则阈值
 * @param op    比较符
 * @param value 实测值
 * @param limit 阈值
 * @return RT_TRUE 命中（越界）
 */
static rt_bool_t rule_compare(const char *op, double value, double limit)
{
    if (rt_strcmp(op, ">") == 0)  return (value > limit) ? RT_TRUE : RT_FALSE;
    if (rt_strcmp(op, ">=") == 0) return (value >= limit) ? RT_TRUE : RT_FALSE;
    if (rt_strcmp(op, "<") == 0)  return (value < limit) ? RT_TRUE : RT_FALSE;
    if (rt_strcmp(op, "<=") == 0) return (value <= limit) ? RT_TRUE : RT_FALSE;
    if (rt_strcmp(op, "==") == 0) return (value == limit) ? RT_TRUE : RT_FALSE;
    if (rt_strcmp(op, "!=") == 0) return (value != limit) ? RT_TRUE : RT_FALSE;
    return RT_FALSE;
}

/*
 * @brief 阈值规则判定（含边沿触发与告警冷却）
 * @note  仅在「由未越界变为越界」或「冷却时间已过」时放行，
 *        避免持续越界期间每条上报都打扰 agent
 * @param item   消息
 * @param reason 命中原因（输出）
 * @param size   原因缓冲大小
 * @return RT_TRUE 需要交给 agent
 */
rt_bool_t rule_gate(const mqtt_rx_item_t *item, char *reason, rt_size_t size)
{
    int i;
    rt_bool_t forward = RT_FALSE;

    if (!lock_take())
    {
        return RT_TRUE; /* 状态锁异常时保守放行，避免漏掉重要消息 */
    }

    for (i = 0; i < MQTT_TOOL_MAX_RULES; i++)
    {
        mqtt_rule_entry_t *rule = &s_rules[i];
        double value = 0.0;

        if (!rule->used || !topic_filter_match(rule->topic, item->topic))
        {
            continue;
        }

        /* 支持嵌套路径/数组/通配，命中任一候选值即视为越界 */
        if (!payload_match_rule(item->payload, rule->field, rule->op, rule->value, &value))
        {
            /* 未命中：清除边沿标记，下次越界可再次告警 */
            rule->triggered = RT_FALSE;
            continue;
        }

        {
            rt_bool_t cooled = RT_TRUE;
            rt_tick_t cooldown = rt_tick_from_millisecond(PKG_AGENT_TOOL_MQTT_ALERT_COOLDOWN_MS);

            if (rule->triggered && (rt_tick_get() - rule->last_alert_tick) < cooldown)
            {
                cooled = RT_FALSE;
            }

            if (!rule->triggered || cooled)
            {
                rule->last_alert_tick = rt_tick_get();
                rule->alert_count++;
                s_alert_total++;
                forward = RT_TRUE;
                if (reason != RT_NULL && size > 0)
                {
                    rt_snprintf(reason, size, "threshold rule matched: %s%s %g %s %g",
                                (rule->field[0] != '\0') ? rule->field : "payload",
                                (rule->field[0] != '\0') ? "=" : "",
                                value, rule->op, rule->value);
                }
            }
            rule->triggered = RT_TRUE;
        }

        if (forward)
        {
            break;
        }
    }

    lock_give();
    return forward;
}

/*
 * @brief 设置/清除阈值规则
 * @param action  "set" 设置，其它值（"clear"/"delete"）清除
 * @param topic   话题过滤器
 * @param field   JSON 字段名（可为空：整条 payload 视为数值）
 * @param op      比较符(">", ">=", "<", "<=", "==", "!=")
 * @param value   阈值
 * @param out     结果文本
 * @param out_size 结果缓冲大小
 * @return RT_EOK 成功，RT_ERROR 失败
 */
rt_err_t mqtt_do_rule(const char *action, const char *topic, const char *field,
                             const char *op, double value, char *out, rt_size_t out_size)
{
    rt_size_t used = 0;
    int i;
    int free_idx = -1;

    if (topic == RT_NULL || topic[0] == '\0' || rt_strlen(topic) >= MQTT_TOOL_TOPIC_MAX_LEN)
    {
        buf_append(out, out_size, &used, "error: valid topic is required");
        return RT_ERROR;
    }
    if (field != RT_NULL && rt_strlen(field) >= MQTT_TOOL_FIELD_MAX_LEN)
    {
        buf_append(out, out_size, &used, "error: field too long (max %d)", MQTT_TOOL_FIELD_MAX_LEN - 1);
        return RT_ERROR;
    }
    if (op == RT_NULL || op[0] == '\0')
    {
        op = ">";
    }
    if (rt_strcmp(op, ">") != 0 && rt_strcmp(op, ">=") != 0 && rt_strcmp(op, "<") != 0 &&
        rt_strcmp(op, "<=") != 0 && rt_strcmp(op, "==") != 0 && rt_strcmp(op, "!=") != 0)
    {
        buf_append(out, out_size, &used, "error: op must be one of > >= < <= == !=");
        return RT_ERROR;
    }

    if (!lock_take())
    {
        buf_append(out, out_size, &used, "error: mqtt state busy, retry later");
        return RT_ERROR;
    }

    /* 先找同一个 (topic, field, op) 的已有规则：命中则更新，否则新建 */
    for (i = 0; i < MQTT_TOOL_MAX_RULES; i++)
    {
        if (s_rules[i].used &&
            rt_strcmp(s_rules[i].topic, topic) == 0 &&
            rt_strcmp(s_rules[i].field, (field != RT_NULL) ? field : "") == 0 &&
            rt_strcmp(s_rules[i].op, op) == 0)
        {
            break;
        }
        if (!s_rules[i].used && free_idx < 0)
        {
            free_idx = i;
        }
    }

    if (rt_strcmp(action, "clear") == 0 || rt_strcmp(action, "delete") == 0)
    {
        /* 清除：topic 为 "*" 时清空全部，否则清除该话题下的规则 */
        int removed = 0;
        for (i = 0; i < MQTT_TOOL_MAX_RULES; i++)
        {
            if (!s_rules[i].used)
            {
                continue;
            }
            if (rt_strcmp(topic, "*") == 0 || topic_filter_match(topic, s_rules[i].topic))
            {
                rt_memset(&s_rules[i], 0, sizeof(s_rules[i]));
                removed++;
            }
        }
        lock_give();
        buf_append(out, out_size, &used, "rule clear: %d rule(s) removed", removed);
        return RT_EOK;
    }

    if (i >= MQTT_TOOL_MAX_RULES && free_idx < 0)
    {
        lock_give();
        buf_append(out, out_size, &used, "error: rule table full (max %d)", MQTT_TOOL_MAX_RULES);
        return RT_ERROR;
    }
    if (i >= MQTT_TOOL_MAX_RULES)
    {
        i = free_idx;
        rt_memset(&s_rules[i], 0, sizeof(s_rules[i]));
        rt_strncpy(s_rules[i].topic, topic, MQTT_TOOL_TOPIC_MAX_LEN - 1);
        if (field != RT_NULL)
        {
            rt_strncpy(s_rules[i].field, field, MQTT_TOOL_FIELD_MAX_LEN - 1);
        }
        rt_strncpy(s_rules[i].op, op, sizeof(s_rules[i].op) - 1);
        s_rules[i].used = RT_TRUE;
    }

    s_rules[i].value = value;
    s_rules[i].triggered = RT_FALSE;   /* 重新设定阈值后重新做边沿判定 */

    buf_append(out, out_size, &used,
               "rule set: topic=%s path=%s%s %s %g (cooldown %d ms)\n"
               "you will only be notified when this condition becomes true (or after the cooldown)",
               topic,
               (s_rules[i].field[0] != '\0') ? s_rules[i].field : "payload",
               (s_rules[i].field[0] != '\0') ? "" : " (whole payload as number)",
               s_rules[i].op, s_rules[i].value,
               PKG_AGENT_TOOL_MQTT_ALERT_COOLDOWN_MS);
    lock_give();
    return RT_EOK;
}

/*
 * @brief 取出轮询缓冲中的消息（供 poll 模式与手动排查使用）
 * @param max          最多取出条数
 * @param topic_filter 话题过滤（NULL/空表示不过滤，支持通配）
 * @param out          结果文本
 * @param out_size     结果缓冲大小
 * @return 实际取出条数
 */
