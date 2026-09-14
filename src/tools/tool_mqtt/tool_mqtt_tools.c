/*
 * MQTT 工具 - LLM 工具入口（mqtt_publish / mqtt_subscribe / mqtt_rule / mqtt_receive）
 * 把工具参数 JSON 映射为业务操作，并把结果写回工具节点。
 *
 * 说明：本模块由原 tool_mqtt.c 拆分而来，模块间共享的状态与接口
 *       统一声明在 tool_mqtt_internal.h（对外接口见 tool_mqtt.h）。
 */
#include "tool_mqtt_internal.h"

#define LOG_TAG "Agent.tool_mqtt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* ==========================================================================
 * 工具执行函数（注册到 agent 工具链表）
 * ========================================================================== */

/*
 * @brief 统一写入工具返回结果
 * @param node 工具节点
 * @param ret  业务返回码
 * @param text 结果文本
 */
static void tool_set_result(AgentToolNode_t node, rt_err_t ret, const char *text)
{
    node->ret.messages = messages_create(1);
    if (node->ret.messages == RT_NULL)
    {
        node->ret.ret = RT_ERROR;
        return;
    }
    if (messages_append(node->ret.messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
        node->ret.ret = RT_ERROR;
        return;
    }
    node->ret.ret = ret;
}

/*
 * @brief 工具通用前置处理：参数校验 + 释放上次结果
 * @param args_obj 参数 JSON 对象
 * @param node     工具节点
 * @return RT_TRUE 可以继续执行
 */
static rt_bool_t tool_prepare(cJSON *args_obj, AgentToolNode_t node)
{
    if (node == RT_NULL)
    {
        return RT_FALSE;
    }

    /* 释放上一次调用的旧结果（节点会被复用） */
    if (node->ret.messages)
    {
        messages_destroy(node->ret.messages);
        node->ret.messages = RT_NULL;
    }

    if (args_obj == RT_NULL)
    {
        node->ret.ret = RT_ERROR;
        return RT_FALSE;
    }

    return RT_TRUE;
}

/*
 * @brief MQTT 发布工具：向话题下发消息/命令
 * @param args_obj 参数 JSON 对象 {topic?: string, message: string}
 * @param node     工具节点
 */
void tool_mqtt_publish(cJSON *args_obj, AgentToolNode_t node)
{
    char result[MQTT_TOOL_RESULT_SMALL] = {0};
    cJSON *topic_json;
    cJSON *msg_json;
    const char *topic = RT_NULL;
    rt_err_t ret;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    msg_json = cJSON_GetObjectItemCaseSensitive(args_obj, "message");
    topic_json = cJSON_GetObjectItemCaseSensitive(args_obj, "topic");

    if (!msg_json || !cJSON_IsString(msg_json) || msg_json->valuestring[0] == '\0')
    {
        tool_set_result(node, RT_ERROR, "error: 'message' (non-empty string) is required");
        return;
    }
    if (topic_json != RT_NULL && cJSON_IsString(topic_json))
    {
        topic = topic_json->valuestring;
    }

    ret = mqtt_do_publish(topic, msg_json->valuestring, result, sizeof(result));
    tool_set_result(node, ret, result);
}

/*
 * @brief MQTT 订阅工具：订阅/退订/查看订阅
 * @param args_obj 参数 JSON 对象 {action?: string, topic?: string, mode?: string}
 * @param node     工具节点
 */
void tool_mqtt_subscribe(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *action_json;
    cJSON *topic_json;
    cJSON *mode_json;
    const char *action = "subscribe";
    const char *topic = RT_NULL;
    mqtt_deliver_mode_t mode = MQTT_DELIVER_FILTER;
    char *result;
    rt_err_t ret;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_MID);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    action_json = cJSON_GetObjectItemCaseSensitive(args_obj, "action");
    topic_json = cJSON_GetObjectItemCaseSensitive(args_obj, "topic");
    mode_json = cJSON_GetObjectItemCaseSensitive(args_obj, "mode");

    if (action_json != RT_NULL && cJSON_IsString(action_json) && action_json->valuestring[0] != '\0')
    {
        action = action_json->valuestring;
    }
    if (topic_json != RT_NULL && cJSON_IsString(topic_json))
    {
        topic = topic_json->valuestring;
    }
    if (mode_json != RT_NULL && cJSON_IsString(mode_json))
    {
        if (rt_strcmp(mode_json->valuestring, "poll") != 0 &&
            rt_strcmp(mode_json->valuestring, "auto") != 0 &&
            rt_strcmp(mode_json->valuestring, "filter") != 0)
        {
            rt_free(result);
            tool_set_result(node, RT_ERROR, "error: 'mode' must be 'filter', 'auto' or 'poll'");
            return;
        }
        mode = mqtt_mode_from_string(mode_json->valuestring);
    }

    if (rt_strcmp(action, "subscribe") == 0)
    {
        ret = mqtt_do_subscribe(topic, mode, result, MQTT_TOOL_RESULT_MID);
    }
    else if (rt_strcmp(action, "unsubscribe") == 0)
    {
        ret = mqtt_do_unsubscribe(topic, result, MQTT_TOOL_RESULT_MID);
    }
    else if (rt_strcmp(action, "list") == 0 || rt_strcmp(action, "status") == 0)
    {
        mqtt_do_list(result, MQTT_TOOL_RESULT_MID);
        ret = RT_EOK;
    }
    else
    {
        ret = RT_ERROR;
        rt_snprintf(result, MQTT_TOOL_RESULT_MID,
                    "error: unknown action '%s' (use subscribe|unsubscribe|list)", action);
    }

    tool_set_result(node, ret, result);
    rt_free(result);
}

/*
 * @brief MQTT 阈值规则工具：设置/清除/查看"越界才打扰 agent"的规则
 * @param args_obj 参数 JSON 对象 {action?: string, topic?: string, field?: string,
 *                                 op?: string, value?: number}
 * @param node     工具节点
 */
void tool_mqtt_rule(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *action_json;
    cJSON *topic_json;
    cJSON *field_json;
    cJSON *op_json;
    cJSON *value_json;
    const char *action = "set";
    const char *topic = RT_NULL;
    const char *field = RT_NULL;
    const char *op = ">";
    double value = 0.0;
    char *result;
    rt_err_t ret;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_MID);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    action_json = cJSON_GetObjectItemCaseSensitive(args_obj, "action");
    topic_json = cJSON_GetObjectItemCaseSensitive(args_obj, "topic");
    field_json = cJSON_GetObjectItemCaseSensitive(args_obj, "field");
    op_json = cJSON_GetObjectItemCaseSensitive(args_obj, "op");
    value_json = cJSON_GetObjectItemCaseSensitive(args_obj, "value");

    if (action_json != RT_NULL && cJSON_IsString(action_json) && action_json->valuestring[0] != '\0')
    {
        action = action_json->valuestring;
    }
    if (topic_json != RT_NULL && cJSON_IsString(topic_json))
    {
        topic = topic_json->valuestring;
    }
    if (field_json != RT_NULL && cJSON_IsString(field_json))
    {
        field = field_json->valuestring;
    }
    if (op_json != RT_NULL && cJSON_IsString(op_json) && op_json->valuestring[0] != '\0')
    {
        op = op_json->valuestring;
    }
    if (value_json != RT_NULL && cJSON_IsNumber(value_json))
    {
        value = value_json->valuedouble;
    }

    if (rt_strcmp(action, "list") == 0 || rt_strcmp(action, "status") == 0)
    {
        mqtt_do_list(result, MQTT_TOOL_RESULT_MID);
        tool_set_result(node, RT_EOK, result);
        rt_free(result);
        return;
    }

    if (rt_strcmp(action, "clear") != 0 && rt_strcmp(action, "delete") != 0 &&
        value_json == RT_NULL)
    {
        rt_free(result);
        tool_set_result(node, RT_ERROR,
                        "error: 'value' (number) is required to set a rule; "
                        "use action=clear to remove rules");
        return;
    }

    ret = mqtt_do_rule(action, topic, field, op, value, result, MQTT_TOOL_RESULT_MID);
    tool_set_result(node, ret, result);
    rt_free(result);
}

/*
 * @brief MQTT 收取工具：取出 poll 模式下缓存的消息
 * @param args_obj 参数 JSON 对象 {max?: number, topic?: string}
 * @param node     工具节点
 */
void tool_mqtt_receive(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *max_json;
    cJSON *topic_json;
    const char *topic_filter = RT_NULL;
    int max = MQTT_TOOL_POLL_DEPTH;
    char *result;
    rt_size_t used = 0;
    int n;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    max_json = cJSON_GetObjectItemCaseSensitive(args_obj, "max");
    topic_json = cJSON_GetObjectItemCaseSensitive(args_obj, "topic");

    if (max_json != RT_NULL && cJSON_IsNumber(max_json))
    {
        max = (int)max_json->valuedouble;
    }
    if (topic_json != RT_NULL && cJSON_IsString(topic_json))
    {
        topic_filter = topic_json->valuestring;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_LARGE);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    n = mqtt_do_receive(max, topic_filter, result, MQTT_TOOL_RESULT_LARGE);

    /*
     * mqtt_do_receive 内部用自己的局部 used，不会把写入长度带回来。
     * 这里必须按缓冲区的实际内容长度续写：如果沿用 0，后面的这几行会从偏移 0
     * 开始覆盖，把刚刚取到的消息内容整段吃掉（工具结果只剩尾部提示语）。
     */
    used = rt_strlen(result);
    if (n > 0)
    {
        buf_append(result, MQTT_TOOL_RESULT_LARGE, &used, "total %d message(s) fetched\n", n);
    }

    tool_set_result(node, RT_EOK, result);
    rt_free(result);
}

/*
 * @brief MQTT 近期情况工具：汇总最近消息（含 payload）+ 数值统计，供 agent 评估近期情况
 * @param args_obj 参数 JSON 对象 {topic?: string, max?: number}
 * @param node     工具节点
 */
void tool_mqtt_history(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *topic_json;
    cJSON *max_json;
    const char *topic_filter = RT_NULL;
    int max = MQTT_TOOL_HISTORY_DEPTH;
    char *result;
    rt_size_t used = 0;
    int topics;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    topic_json = cJSON_GetObjectItemCaseSensitive(args_obj, "topic");
    max_json = cJSON_GetObjectItemCaseSensitive(args_obj, "max");

    if (topic_json != RT_NULL && cJSON_IsString(topic_json))
    {
        topic_filter = topic_json->valuestring;
    }
    if (max_json != RT_NULL && cJSON_IsNumber(max_json))
    {
        max = (int)max_json->valuedouble;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_XLARGE);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    topics = mqtt_do_history(topic_filter, max, result, MQTT_TOOL_RESULT_XLARGE);

    /* 同 mqtt_receive：必须从实际内容长度续写，否则提示语会覆盖掉历史正文 */
    used = rt_strlen(result);

    if (topics > 0)
    {
        buf_append(result, MQTT_TOOL_RESULT_XLARGE, &used,
                   "\nSummarize the trend above and give the user a short assessment of the recent "
                   "situation: what changed, whether anything needs attention.");
    }
    else if (rt_strstr(result, "error:") != RT_NULL)
    {
        /*
         * 取锁失败之类：上面的错误文本本身已经说明原因，这里只补一句可执行结论，
         * 不再重复「no topic matched」以免与真实原因矛盾。
         */
        buf_append(result, MQTT_TOOL_RESULT_XLARGE, &used,
                   "\n(the query could not run - see the error above; retry once)\n");
    }

    tool_set_result(node, RT_EOK, result);
    rt_free(result);
}

/*
 * @brief MQTT 连接工具：确保客户端已启动并连上 broker（可显式指定等待超时）
 * @param args_obj 参数 JSON 对象 {timeout_ms?: number}
 * @param node     工具节点
 */
void tool_mqtt_connect(cJSON *args_obj, AgentToolNode_t node)
{
    cJSON *timeout_json;
    rt_int32_t timeout_ms = 0;
    char *result;
    rt_err_t ret;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    timeout_json = cJSON_GetObjectItemCaseSensitive(args_obj, "timeout_ms");
    if (timeout_json != RT_NULL && cJSON_IsNumber(timeout_json))
    {
        timeout_ms = (rt_int32_t)timeout_json->valuedouble;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_MID);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    ret = mqtt_do_connect(timeout_ms, result, MQTT_TOOL_RESULT_MID);
    tool_set_result(node, ret, result);
    rt_free(result);
}

/*
 * @brief MQTT 断开工具：断开连接并释放工作线程/接收线程/队列
 * @note  订阅意图保留，下次连接后自动重新订阅
 * @param args_obj 参数 JSON 对象（可为空）
 * @param node     工具节点
 */
void tool_mqtt_disconnect(cJSON *args_obj, AgentToolNode_t node)
{
    char *result;

    if (!tool_prepare(args_obj, node))
    {
        return;
    }

    result = rt_malloc(MQTT_TOOL_RESULT_MID);
    if (result == RT_NULL)
    {
        tool_set_result(node, RT_ERROR, "error: out of memory");
        return;
    }
    result[0] = '\0';

    mqtt_do_disconnect(result, MQTT_TOOL_RESULT_MID);
    tool_set_result(node, RT_EOK, result);
    rt_free(result);
}

