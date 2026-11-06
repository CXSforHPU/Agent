#ifndef __AGENT_TOOL_TOOLS_H__
#define __AGENT_TOOL_TOOLS_H__

#include "tool_add.h"
#include "tool_mul.h"
#include "tool_compare.h"

/* MQTT 工具：发布话题命令 / 订阅话题并交给 agent 分析（随 Kconfig 开关启用） */
#ifdef PKG_AGENT_TOOL_MQTT_ENABLE
#include "tool_mqtt.h"
#endif


#endif // __AGENT_TOOL_TOOLS_H__