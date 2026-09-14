#include "tool_func.h"

static rt_err_t init_flag = RT_FALSE;

/*
 * @brief 初始化所有工具（注册到工具链表）
 */
void init_tools(void)
{
    if (init_flag)
    {
        return;
    }

    agent_tool_list_create();

    /* ========== add ========== */
    cJSON *props_add = cJSON_CreateObject();
    create_property(props_add, "a", "double", "First number");
    create_property(props_add, "b", "double", "Second number");

    cJSON *required_add = cJSON_CreateArray();
    cJSON_AddItemToArray(required_add, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_add, cJSON_CreateString("b"));

    cJSON *params_add = create_param_obj(props_add, required_add);
    cJSON *add_func = create_tool_item("add", "Calculate the sum of two numbers", params_add);
    append_tool(add_func, tool_add);

    /* ========== mul ========== */
    cJSON *props_mul = cJSON_CreateObject();
    create_property(props_mul, "a", "double", "First multiplier");
    create_property(props_mul, "b", "double", "Second multiplier");

    cJSON *required_mul = cJSON_CreateArray();
    cJSON_AddItemToArray(required_mul, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_mul, cJSON_CreateString("b"));

    cJSON *params_mul = create_param_obj(props_mul, required_mul);
    cJSON *mul_func = create_tool_item("mul", "Calculate the product of two numbers", params_mul);
    append_tool(mul_func, tool_mul);

    /* ========== compare ========== */
    cJSON *props_cmp = cJSON_CreateObject();
    create_property(props_cmp, "a", "double", "Number A to compare");
    create_property(props_cmp, "b", "double", "Number B to compare");

    cJSON *required_cmp = cJSON_CreateArray();
    cJSON_AddItemToArray(required_cmp, cJSON_CreateString("a"));
    cJSON_AddItemToArray(required_cmp, cJSON_CreateString("b"));

    cJSON *params_cmp = create_param_obj(props_cmp, required_cmp);
    cJSON *cmp_func = create_tool_item("compare", "Compare the size of two numbers", params_cmp);
    append_tool(cmp_func, tool_compare);

#ifdef PKG_AGENT_TOOL_MQTT_ENABLE
    /* ========== mqtt_connect ========== */
    /* 连接 broker：未启动则启动，未连上则等待（发布/订阅内部也会自动做这一步） */
    cJSON *props_mqtt_conn = cJSON_CreateObject();
    create_property(props_mqtt_conn, "timeout_ms", "double",
                    "How long to wait for the connection, in milliseconds "
                    "(default: the built-in connect wait).");

    cJSON *required_mqtt_conn = cJSON_CreateArray();

    cJSON *params_mqtt_conn = create_param_obj(props_mqtt_conn, required_mqtt_conn);
    cJSON *mqtt_conn_func = create_tool_item(
        "mqtt_connect",
        "Connect the MQTT client to the broker (starts it if needed) and report the connection "
        "state. Use it when the user asks to connect/go online, or to diagnose why publishing or "
        "subscribing fails. publish/subscribe already perform this check automatically.",
        params_mqtt_conn);
    append_tool(mqtt_conn_func, tool_mqtt_connect);

    /* ========== mqtt_disconnect ========== */
    /* 断开连接：订阅意图保留，下次连接后自动重新订阅 */
    cJSON *props_mqtt_disc = cJSON_CreateObject();
    cJSON *required_mqtt_disc = cJSON_CreateArray();

    cJSON *params_mqtt_disc = create_param_obj(props_mqtt_disc, required_mqtt_disc);
    cJSON *mqtt_disc_func = create_tool_item(
        "mqtt_disconnect",
        "Disconnect the MQTT client and release its worker/receive threads. Subscriptions are kept "
        "and will be re-subscribed automatically on the next mqtt_connect. Use it when the user asks "
        "to go offline, stop MQTT traffic, or before reconfiguring the broker.",
        params_mqtt_disc);
    append_tool(mqtt_disc_func, tool_mqtt_disconnect);

    /* ========== mqtt_publish ========== */
    /* 向 MQTT 话题发布消息（下发命令）。topic 省略时使用默认发布话题 */
    cJSON *props_mqtt_pub = cJSON_CreateObject();
    create_property(props_mqtt_pub, "topic", "string",
                    "Target MQTT topic, e.g. 'device/cmd'. Omit to use the default publish topic.");
    create_property(props_mqtt_pub, "message", "string",
                    "Message payload to publish (plain text or a JSON string).");

    cJSON *required_mqtt_pub = cJSON_CreateArray();
    cJSON_AddItemToArray(required_mqtt_pub, cJSON_CreateString("message"));

    cJSON *params_mqtt_pub = create_param_obj(props_mqtt_pub, required_mqtt_pub);
    cJSON *mqtt_pub_func = create_tool_item(
        "mqtt_publish",
        "Publish a message to an MQTT topic to send a command to a device or service. "
        "Use this when the user asks to send/issue a command over MQTT. "
        "Returns the publish result including topic and payload byte length.",
        params_mqtt_pub);
    append_tool(mqtt_pub_func, tool_mqtt_publish);

    /* ========== mqtt_subscribe ========== */
    /* 订阅/退订/查看 MQTT 话题；auto 模式下收到的消息会自动交给 agent 分析 */
    cJSON *props_mqtt_sub = cJSON_CreateObject();
    create_property(props_mqtt_sub, "action", "string",
                    "One of 'subscribe' (default), 'unsubscribe', 'list'.");
    create_property(props_mqtt_sub, "topic", "string",
                    "Topic filter to subscribe/unsubscribe, wildcards '+' and '#' supported.");
    create_property(props_mqtt_sub, "mode", "string",
                    "'filter' (default): only forward messages that look like a question or match an "
                    "mqtt_rule threshold, everything else stays local; "
                    "'auto': forward every message to you; 'poll': only buffer them.");

    cJSON *required_mqtt_sub = cJSON_CreateArray();
    cJSON_AddItemToArray(required_mqtt_sub, cJSON_CreateString("action"));

    cJSON *params_mqtt_sub = create_param_obj(props_mqtt_sub, required_mqtt_sub);
    cJSON *mqtt_sub_func = create_tool_item(
        "mqtt_subscribe",
        "Subscribe to an MQTT topic to watch it, or unsubscribe / list current subscriptions. "
        "In the default 'filter' mode you are only woken up when a message needs advice or a "
        "threshold rule (see mqtt_rule) is exceeded; routine readings are filtered locally.",
        params_mqtt_sub);
    append_tool(mqtt_sub_func, tool_mqtt_subscribe);

    /* ========== mqtt_receive ========== */
    /* 取出 poll 模式缓存的消息 */
    cJSON *props_mqtt_recv = cJSON_CreateObject();
    create_property(props_mqtt_recv, "max", "double",
                    "Maximum number of buffered messages to fetch (up to 8).");
    create_property(props_mqtt_recv, "topic", "string",
                    "Only fetch messages of this topic (wildcards supported).");

    cJSON *required_mqtt_recv = cJSON_CreateArray();

    cJSON *params_mqtt_recv = create_param_obj(props_mqtt_recv, required_mqtt_recv);
    cJSON *mqtt_recv_func = create_tool_item(
        "mqtt_receive",
        "Fetch MQTT messages buffered by topics that are not auto-forwarded (filter/poll mode). "
        "Use it when the user asks for recent readings or the current value of a topic.",
        params_mqtt_recv);
    append_tool(mqtt_recv_func, tool_mqtt_receive);

    /* ========== mqtt_rule ========== */
    /* 阈值规则：只有越界（或冷却到期）时才把消息交给 agent 总结/告警 */
    cJSON *props_mqtt_rule = cJSON_CreateObject();
    create_property(props_mqtt_rule, "action", "string",
                    "One of 'set' (default), 'clear' (remove rules), 'list'.");
    create_property(props_mqtt_rule, "topic", "string",
                    "Topic filter the rule applies to (wildcards supported; '*' clears all).");
    create_property(props_mqtt_rule, "field", "string",
                    "JSON field path to compare. Nested objects use '.', arrays use '[n]' and '[*]' "
                    "(any element). Examples: 'hum', 'sensor.hum', 'readings[0].t', 'readings[*].temp'. "
                    "Omit it when the whole payload is a bare number (e.g. '35').");
    create_property(props_mqtt_rule, "op", "string",
                    "Comparison operator: '>', '>=', '<', '<=', '==' or '!=' (default '>').");
    create_property(props_mqtt_rule, "value", "double",
                    "Threshold value, e.g. 30.");

    cJSON *required_mqtt_rule = cJSON_CreateArray();
    cJSON_AddItemToArray(required_mqtt_rule, cJSON_CreateString("action"));

    cJSON *params_mqtt_rule = create_param_obj(props_mqtt_rule, required_mqtt_rule);
    cJSON *mqtt_rule_func = create_tool_item(
        "mqtt_rule",
        "Manage threshold rules so you are only notified when a subscribed value is out of range "
        "(normal readings are filtered locally and never wake you up). "
        "Example: set topic='agent/sub', field='hum', op='>', value=30 to be told when humidity "
        "goes above 30. Use it whenever the user asks to 'watch', 'monitor' or 'alert me when ...'.",
        params_mqtt_rule);
    append_tool(mqtt_rule_func, tool_mqtt_rule);

    /* ========== mqtt_history ========== */
    /* 近期情况汇总：最近消息（含 payload）+ 数值字段统计，只读不消费 */
    cJSON *props_mqtt_hist = cJSON_CreateObject();
    create_property(props_mqtt_hist, "topic", "string",
                    "Topic filter to summarize; omit to summarize every subscribed topic.");
    create_property(props_mqtt_hist, "max", "double",
                    "How many recent messages to list per topic (default 8).");

    cJSON *required_mqtt_hist = cJSON_CreateArray();

    cJSON *params_mqtt_hist = create_param_obj(props_mqtt_hist, required_mqtt_hist);
    cJSON *mqtt_hist_func = create_tool_item(
        "mqtt_history",
        "Summarize what happened recently on subscribed MQTT topics: the latest messages with their "
        "raw payloads, min/max/average/last of every numeric field, and how many messages were "
        "received/forwarded/filtered. Read-only (it does not consume anything). "
        "Use this whenever the user asks how things have been, wants a trend, or asks you to assess "
        "the recent situation of a sensor/topic.",
        params_mqtt_hist);
    append_tool(mqtt_hist_func, tool_mqtt_history);
#endif /* PKG_AGENT_TOOL_MQTT_ENABLE */

    /* Build tool list json */
    build_tools_json();
    init_flag = RT_TRUE;
}

/*
 * @brief 清理所有已注册工具并复位注册标志
 * @note 由 cleanup_agent 调用；再次进入对话时 init_tools() 会重新注册工具
 */
void agent_tools_cleanup(void)
{
    agent_tool_list_destroy();
    init_flag = RT_FALSE;
}

MSH_CMD_EXPORT(init_tools, init_tools);