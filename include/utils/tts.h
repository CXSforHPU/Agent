#ifndef __AGENT_UTILS_TTS_H__
#define __AGENT_UTILS_TTS_H__

#include "rtconfig.h"

#ifdef PKG_AGENT_TTS_ENABLE

/* 合成结果默认落盘位置，msh_agent_tts 与播报线程共用 */
#define AGENT_TTS_SAVE_PATH     "/tts.wav"
/* 响应体按这个大小分块写盘，不整段读进内存 */
#define AGENT_TTS_IO_BUF_SZ     2048
/* 让服务端重采样到板上采样率，省一半字节 */
#define AGENT_TTS_SAMPLE_RATE   16000
/* AACI/AC97 一帧固定携带左右两个 PCM 槽。喂单声道数据时，wavplay 会把相邻
   两个采样当成一帧，播放速度和音调都翻倍，所以这里要立体声。 */
#define AGENT_TTS_CHANNELS      2
/* 服务端没预热时，首帧要先加载模型再做一次即时编译，实测能到 180 s，超时给到
   5 min；模型常驻后同样一句只要 4 s 左右。 */
#define AGENT_TTS_TIMEOUT_MS    300000

/*
 * 合成入口 agent_tts_synth() 是 tts.c 的静态函数，只服务 msh_agent_tts 和播报线程，
 * 这里只暴露回复播报。
 */
#ifdef PKG_AGENT_TTS_AUTO_PLAY

/* 一条回复的文本上限，超出部分由服务端截断，板上按 UTF-8 字符边界切 */
#define AGENT_TTS_SPEAK_TEXT_SZ   512
/* webclient 会话与收发缓冲都在堆上，8 K 栈是照 fileop_wk（4 K）加上一句回复的余量 */
#define AGENT_TTS_SPEAK_STACK     8192

void agent_tts_speak_init(void);
void agent_tts_speak(const char *text);

#endif // PKG_AGENT_TTS_AUTO_PLAY

#endif // PKG_AGENT_TTS_ENABLE

#endif // __AGENT_UTILS_TTS_H__
