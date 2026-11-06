#ifndef __AGENT_UTILS_STT_H__
#define __AGENT_UTILS_STT_H__

#include "rtconfig.h"

#ifdef PKG_AGENT_STT_ENABLE

/* multipart 收发缓冲，录音文件按这个大小分片送出 */
#define AGENT_STT_IO_BUF_SZ   1024
/* 首次识别要加载 whisper 模型，可能耗时数十秒，连上之后把收发超时放宽到这里 */
#define AGENT_STT_TIMEOUT_MS  120000

/*
 * @brief 把本地 WAV 发给 STT 服务转成文本
 * @param path 本地 WAV 文件路径
 * @return 转写文本，指向静态缓冲区，调用者需及时拷贝；失败返回 RT_NULL
 */
const char *agent_stt_transcribe(const char *path);

#endif // PKG_AGENT_STT_ENABLE

#endif // __AGENT_UTILS_STT_H__
