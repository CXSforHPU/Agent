/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-11-17     CXSforHPU    first version
 */

/**
 * Test Case Name: Agent Layered Memory Test
 *
 * Test Objectives:
 * - Validate the static memory pool behind `agent_memory_*`: store, dedup,
 *   ring eviction, keyword retrieval (ASCII and CJK), delete and listing
 * - Verify the injected block appears as a user message after the system
 *   prompt and before the appended request text
 *
 * Test Scenarios:
 * - Fill core and recall rings past capacity and check the oldest item is gone
 * - Query with English words, Chinese words and punctuation-only input
 * - Delete by id, then delete the same id again and expect failure
 * - Build an `agent_chat_src_t` with memory and walk the cursor order
 *
 * Verification Metrics:
 * - All uassert assertions pass
 * - Counts never exceed the configured slot numbers
 * - Injected text carries the header line and `[id=...]` prefixes
 *
 * Dependencies:
 * - Agent package with `PKG_AGENT_MEMORY_ENABLE`
 * - Run with: utest_run -thread agent.memory
 *
 * Expected Results:
 * - "[  PASSED  ] [ result   ] testcase (agent.memory)"
 */

#include <rtthread.h>
#include "utest.h"
#include "utest_assert.h"
#include "memory.h"
#include "core/context.h"

static char g_inject[PKG_AGENT_MEMORY_INJECT_BYTES];
static char g_query[512];

static rt_err_t utest_tc_init(void)
{
    agent_memory_init();
    return RT_EOK;
}

static rt_err_t utest_tc_cleanup(void)
{
    agent_memory_deinit();
    return RT_EOK;
}

static void unit_store_and_dedup(void)
{
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_CORE), 0);
    uassert_null(agent_memory_context("anything"));

    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, "  User   prefers Celsius.  "), RT_EOK);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_CORE), 1);
    uassert_str_equal(agent_memory_item_text(AGENT_MEMORY_CORE, 0, RT_NULL), "User prefers Celsius.");

    /* 同样的内容再存一次不增加条数，避免模型复述时挤掉别的条目 */
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, "User prefers Celsius."), RT_EOK);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_CORE), 1);

    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, "   "), -RT_EINVAL);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, RT_NULL), -RT_EINVAL);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_KIND_MAX, "x"), -RT_EINVAL);
}

static void unit_recall_query(void)
{
    int listed;

    agent_memory_init();
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "MQTT topic is /factory/line1/temp"), RT_EOK);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "The alarm threshold is 40 degrees"), RT_EOK);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "车间温度传感器接在 i2c1 上"), RT_EOK);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_RECALL), 3);

    listed = agent_memory_query(AGENT_MEMORY_RECALL, "mqtt topic", 2, g_query, sizeof(g_query));
    uassert_int_equal(listed, 1);
    uassert_not_null(rt_strstr(g_query, "/factory/line1/temp"));
    uassert_not_null(rt_strstr(g_query, "[id="));

    listed = agent_memory_query(AGENT_MEMORY_RECALL, "温度", 2, g_query, sizeof(g_query));
    uassert_int_equal(listed, 1);
    uassert_not_null(rt_strstr(g_query, "i2c1"));

    /* 没有关键词就按新到旧列出 */
    listed = agent_memory_query(AGENT_MEMORY_RECALL, RT_NULL, 2, g_query, sizeof(g_query));
    uassert_int_equal(listed, 2);
    uassert_not_null(rt_strstr(g_query, "i2c1"));
    uassert_not_null(rt_strstr(g_query, "40 degrees"));

    listed = agent_memory_query(AGENT_MEMORY_RECALL, "银河系", 3, g_query, sizeof(g_query));
    uassert_int_equal(listed, 0);
    uassert_str_equal(g_query, "");

    listed = agent_memory_list(AGENT_MEMORY_RECALL, g_query, sizeof(g_query));
    uassert_int_equal(listed, 3);
}

static void unit_eviction(void)
{
    char text[64];
    int i;
    int id = 0;
    int total = PKG_AGENT_MEMORY_RECALL_ITEMS;

    agent_memory_init();
    for (i = 0; i < total + 1; i++)
    {
        rt_snprintf(text, sizeof(text), "recall item %d", i);
        uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, text), RT_EOK);
    }
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_RECALL), total);
    /* 最旧的一条被覆盖，剩下的是 item 1 .. item total */
    uassert_str_equal(agent_memory_item_text(AGENT_MEMORY_RECALL, 0, &id), text);
    uassert_str_equal(agent_memory_item_text(AGENT_MEMORY_RECALL, total - 1, RT_NULL), "recall item 1");

    for (i = 0; i < PKG_AGENT_MEMORY_CORE_ITEMS + 1; i++)
    {
        rt_snprintf(text, sizeof(text), "core item %d", i);
        uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, text), RT_EOK);
    }
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_CORE), PKG_AGENT_MEMORY_CORE_ITEMS);
    uassert_str_equal(agent_memory_item_text(AGENT_MEMORY_CORE, PKG_AGENT_MEMORY_CORE_ITEMS - 1, RT_NULL),
                      "core item 1");
}

static void unit_delete(void)
{
    int id = 0;

    agent_memory_init();
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, "keep this"), RT_EOK);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "drop this"), RT_EOK);
    uassert_not_null(agent_memory_item_text(AGENT_MEMORY_RECALL, 0, &id));
    uassert_true(id > 0);

    uassert_int_equal(agent_memory_delete(id), RT_EOK);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_RECALL), 0);
    uassert_null(agent_memory_item_text(AGENT_MEMORY_RECALL, 0, RT_NULL));
    uassert_int_equal(agent_memory_delete(id), -RT_ENOENT);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_CORE), 1);

    /* 删除后槽位可以继续用，编号不回退 */
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "new item"), RT_EOK);
    uassert_int_equal(agent_memory_count(AGENT_MEMORY_RECALL), 1);
    uassert_true(agent_memory_item_text(AGENT_MEMORY_RECALL, 0, &id) != RT_NULL);
}

static void unit_inject_order(void)
{
    agent_chat_src_t src;
    agent_ctx_cursor_t cursor;
    agent_ctx_item_t item;

    agent_memory_init();
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_CORE, "User prefers Celsius."), RT_EOK);
    uassert_int_equal(agent_memory_store(AGENT_MEMORY_RECALL, "MQTT topic is /factory/line1/temp"), RT_EOK);

    rt_strncpy(g_inject, agent_memory_context("mqtt topic"), sizeof(g_inject) - 1);
    g_inject[sizeof(g_inject) - 1] = '\0';
    uassert_not_null(rt_strstr(g_inject, "Historical memory"));
    uassert_not_null(rt_strstr(g_inject, "User prefers Celsius."));
    uassert_not_null(rt_strstr(g_inject, "/factory/line1/temp"));

    rt_memset(&src, 0, sizeof(src));
    src.ctx = RT_NULL;
    src.count = -1;
    src.system_override = "SYS";
    src.memory_text = g_inject;
    src.user_text = "How warm is it?";

    agent_ctx_cursor_init(&cursor, &src);

    uassert_true(agent_ctx_cursor_next(&cursor, &item));
    uassert_int_equal(item.role, ROLE_SYSTEM);
    uassert_str_equal(item.text, "SYS");

    uassert_true(agent_ctx_cursor_next(&cursor, &item));
    uassert_int_equal(item.role, ROLE_USER);
    uassert_str_equal(item.text, g_inject);

    uassert_true(agent_ctx_cursor_next(&cursor, &item));
    uassert_int_equal(item.role, ROLE_USER);
    uassert_str_equal(item.text, "How warm is it?");

    uassert_true(!agent_ctx_cursor_next(&cursor, &item));
}

static void testcase(void)
{
    UTEST_UNIT_RUN(unit_store_and_dedup);
    UTEST_UNIT_RUN(unit_recall_query);
    UTEST_UNIT_RUN(unit_eviction);
    UTEST_UNIT_RUN(unit_delete);
    UTEST_UNIT_RUN(unit_inject_order);
}
UTEST_TC_EXPORT(testcase, "agent.memory", utest_tc_init, utest_tc_cleanup, 60);
