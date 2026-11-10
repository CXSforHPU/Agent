#ifndef __AGENT_UTILS_INTERNAL_H__
#define __AGENT_UTILS_INTERNAL_H__

/*
 * Utils 模块收集头
 * src/utils/ 下每个 .c 对应一个同名 .h，这里按启用的宏把它们导进来。
 * 包内其它模块请 include utils.h，不要直接 include 本文件。
 *
 * 模块划分：
 *   message.c     流式打印与文本注入
 *   format.c      Messages 转 API content
 *   tool_json.c   工具定义 JSON 拼装
 *   protocol/     LLM 协议模块，按 OpenAI 与 Anthropic 分文件，统一入口 utils/protocol.h
 *   webclient_util.c  webclient 超时设置，绕开 v2.2.0 的两个坑
 *   file_op.c     文件上传下载线程，需要 PKG_AGENT_MULTIMODAL_ENABLE
 *   stt.c         本地语音转写，需要 PKG_AGENT_STT_ENABLE
 *   tts.c         文本转语音与回复播报，需要 PKG_AGENT_TTS_ENABLE
 */

#include "rtconfig.h"
#include <rtthread.h>
#include "cJSON.h"
#include "webclient.h"
#include "string.h"
#include "MessageHub.h"

#include "utils/message.h"
#include "utils/format.h"
#include "utils/tool_json.h"
#include "utils/protocol.h"
#include "utils/webclient_util.h"

#ifdef PKG_AGENT_MULTIMODAL_ENABLE
#include "utils/file_op.h"
#endif

#ifdef PKG_AGENT_STT_ENABLE
#include "utils/stt.h"
#endif

#ifdef PKG_AGENT_TTS_ENABLE
#include "utils/tts.h"
#endif

#endif // __AGENT_UTILS_INTERNAL_H__
