#ifndef __AGENT_UTILS_TTS_H__
#define __AGENT_UTILS_TTS_H__

#include "rtconfig.h"

#ifdef PKG_AGENT_TTS_ENABLE

/* 合成结果落盘位置，msh_agent_tts 与播报线程共用。必须是可写文件系统：
   根目录挂 romfs 的板子要在 menuconfig 里改到 /sdcard 或 /flash */
#define AGENT_TTS_SAVE_PATH     PKG_AGENT_TTS_SAVE_FILE
/* 响应体按这个大小分块写盘，不整段读进内存。一句长回复的 WAV 有 1~2 MB，
   2 K 的块要跑上千次 read+write 往返，块越大越不容易在中途撞上收包超时 */
#define AGENT_TTS_IO_BUF_SZ     8192
/* 乒乓槽位路径缓冲区：在 AGENT_TTS_SAVE_PATH 的扩展名前插一位序号 */
#define AGENT_TTS_SLOT_PATH_SZ  64
/* 让服务端重采样到板上采样率，省一半字节 */
#define AGENT_TTS_SAMPLE_RATE   16000
/* 板上 I2S 播放线程把送进来的每个采样复制成左右两个 PCM 槽
   （libraries/HAL_Drivers/drv_i2s.c 的 convert_mono_to_stereo()），一个 2048
   字节的块按 64 ms 播完，正好等于 16 kHz 单声道的速率。喂双声道会被当成
   单声道再翻倍，时长拖成两倍、音调低八度，文件与下载流量也跟着翻倍。 */
#define AGENT_TTS_CHANNELS      1
/* 服务端 TTS_AUTOLOAD=1 时启动就热一遍推理，首帧 1 s 级；没热过的进程第一次
   推理要付前端模型和 CUDA 初始化，实测 52~60 s，超时按 5 min 留余量。 */
#define AGENT_TTS_TIMEOUT_MS    300000

/*
 * 合成入口 agent_tts_synth() 是 tts.c 的静态函数，只服务 msh_agent_tts 和播报线程，
 * 这里只暴露回复播报。
 */
#ifdef PKG_AGENT_TTS_AUTO_PLAY

/*
 * 播报走服务端流式：一条回复只发一次 /tts 请求（stream=true），响应体是
 * [4 字节小端长度][完整 WAV] 的帧序列，收到一帧就能播一帧。
 * 整条返回时第一句要白等十几秒（合成约 0.8 s + 每秒语音 0.28 s）；实测一帧 1.5 s
 * 音频的到达时间是 0.9~1.7 s，之后块间隔最长约 1.0 s，比一块的 1.5 s 短，
 * 乒乓槽位只要提前收好一块就不会听见停顿。
 * 块切到 0.5 s 只把起播提前 0.25 s，而最长间隔 0.97 s 已经超过一块的长度，会饿。
 */
#define AGENT_TTS_BLOCK_SECONDS   1.5
/* 一条回复最多收这么多块，防止跑飞的长答案一直占着扬声器（1.5 s/块 ≈ 96 s） */
#define AGENT_TTS_MAX_BLOCKS      64
/* 播报队列深度：一格放一条回复的文本指针，8 条已经排到十几秒之后 */
#define AGENT_TTS_SPEAK_QUEUE     8
/* webclient 会话与收发缓冲都在堆上，8 K 栈是照 fileop_wk（4 K）加上一句回复的余量 */
#define AGENT_TTS_SPEAK_STACK     8192
/* 切换播放槽位前要等上一句播完：合成已经和播放并行了，这里通常几乎不等待，
   超时只是兜底（例如 wavplayer 卡在坏文件上），超时就强制停掉旧播放 */
#define AGENT_TTS_PLAY_WAIT_MS    60000

/*
 * @brief  启动回复播报线程
 */
void agent_tts_speak_init(void);

/*
 * @brief  把一段文本排进播报队列
 * @param  text 要播报的文本
 */
void agent_tts_speak(const char *text);

#endif // PKG_AGENT_TTS_AUTO_PLAY

#endif // PKG_AGENT_TTS_ENABLE

#endif // __AGENT_UTILS_TTS_H__
