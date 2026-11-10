#ifndef __AGENT_AUDIO_H__
#define __AGENT_AUDIO_H__

#include <rtthread.h>

typedef enum
{
    AGENT_WAV_PLAY,
    AGENT_WAV_RECORD,
} AgentWavCmd;

typedef enum
{
    AGENT_WAV_START,
    AGENT_WAV_STOP,
    AGENT_WAV_PAUSE,
    AGENT_WAV_RESUME
} AgentWavOption;

typedef struct
{
    AgentWavCmd cmd;
    AgentWavOption option;
} AgentAudio;

/* 命令字符串映射：头文件只做extern声明，定义放在c文件 */
extern const char* agent_audio_cmd[];
extern const char* agent_audio_option[];

/* 录音默认参数，适配M55硬件 */
#define REC_SAMPLE_RATE     16000
#define REC_CHANNELS        2
#define REC_SAMPLE_BITS     16

/*
 * @brief  播放一个 WAV 文件
 * @param  path 文件路径
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_play(const char* path);

/*
 * @brief  阻塞等当前这条播完
 * @param  timeout_ms 等待毫秒数
 * @return RT_EOK 播完，-RT_ETIMEOUT 超时
 */
int agent_driver_audio_play_wait(rt_int32_t timeout_ms);

/*
 * @brief  停止播放
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_stop(void);

/*
 * @brief  暂停播放
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_pause(void);

/*
 * @brief  恢复播放
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_resume(void);

/*
 * @brief  设置音量
 * @param  vol 音量值，取值范围由驱动决定
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_set_volume(int vol);

/*
 * @brief  开始录音
 * @param  path 录音文件路径
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_record(const char* path);

/*
 * @brief  停止录音
 * @return RT_EOK 成功，负值为错误码
 */
int agent_driver_audio_record_stop(void);

#endif /*__AGENT_AUDIO_H__*/