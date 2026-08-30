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

/* 播放接口，wavplay内部自带线程，调用不会阻塞 */
int agent_driver_audio_play(const char* path);
int agent_driver_audio_stop(void);
int agent_driver_audio_pause(void);
int agent_driver_audio_resume(void);
int agent_driver_audio_set_volume(int vol);

/* 录音：内部新开线程执行，不阻塞调用者线程 */
int agent_driver_audio_record(const char* path);
int agent_driver_audio_record_stop(void);

#endif /*__AGENT_AUDIO_H__*/