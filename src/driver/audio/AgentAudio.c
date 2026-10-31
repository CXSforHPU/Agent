#include "AgentAudio.h"
#include "msh.h"

#ifdef PKG_WP_USING_PLAY
#include "wavplayer.h"
#endif

#define LOG_TAG "Agent.Driver.Audio"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 字符串数组实体，仅本编译单元一份 */
const char* agent_audio_cmd[] =
{
    [AGENT_WAV_PLAY]   = "wavplay",
    [AGENT_WAV_RECORD] = "wavrecord"
};

const char* agent_audio_option[] =
{
    [AGENT_WAV_START]  = "-s",
    [AGENT_WAV_STOP]   = "-t",
    [AGENT_WAV_PAUSE]  = "-p",
    [AGENT_WAV_RESUME] = "-r"
};

#define REC_THREAD_STACK    1024
#define REC_THREAD_PRIO     13

typedef struct
{
    char cmd_buf[128];
} record_thread_ctx_t;

static void record_worker_entry(void *arg)
{
    record_thread_ctx_t *ctx = (record_thread_ctx_t *)arg;
    /* wavrecord阻塞在子线程 */
    msh_exec(ctx->cmd_buf, rt_strlen(ctx->cmd_buf));
    rt_free(ctx);
}

int agent_driver_audio_play(const char* path)
{
    if(path == RT_NULL)
    {
        LOG_E("play path is NULL");
        return -RT_EINVAL;
    }
    char cmd[128] = {0};
    AgentAudio opt = {.cmd = AGENT_WAV_PLAY, .option = AGENT_WAV_START};

    rt_snprintf(cmd, sizeof(cmd), "%s %s %s",
                agent_audio_cmd[opt.cmd],
                agent_audio_option[opt.option],
                path);
    LOG_I("play cmd: %s", cmd);
    return msh_exec(cmd, rt_strlen(cmd));
}

/* wavplayer 起播就返回，播放线程还在读同一个 WAV 文件。合成会把这个文件
   O_TRUNC 重写，所以覆盖之前要等上一句播完，否则正播的那条会被截断。 */
int agent_driver_audio_play_wait(rt_int32_t timeout_ms)
{
#ifndef PKG_WP_USING_PLAY
    return RT_EOK;
#else
    rt_int32_t waited = 0;

    while (wavplayer_state_get() != PLAYER_STATE_STOPED)
    {
        if (waited >= timeout_ms)
        {
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(100);
        waited += 100;
    }
    return RT_EOK;
#endif
}

int agent_driver_audio_stop(void)
{
    char cmd[64] = {0};
    AgentAudio opt = {.cmd = AGENT_WAV_PLAY, .option = AGENT_WAV_STOP};
    rt_snprintf(cmd, sizeof(cmd), "%s %s",
                agent_audio_cmd[opt.cmd],
                agent_audio_option[opt.option]);
    LOG_I("play stop cmd: %s", cmd);
    return msh_exec(cmd, rt_strlen(cmd));
}

int agent_driver_audio_pause(void)
{
    char cmd[64] = {0};
    AgentAudio opt = {.cmd = AGENT_WAV_PLAY, .option = AGENT_WAV_PAUSE};
    rt_snprintf(cmd, sizeof(cmd), "%s %s",
                agent_audio_cmd[opt.cmd],
                agent_audio_option[opt.option]);
    return msh_exec(cmd, rt_strlen(cmd));
}

int agent_driver_audio_resume(void)
{
    char cmd[64] = {0};
    AgentAudio opt = {.cmd = AGENT_WAV_PLAY, .option = AGENT_WAV_RESUME};
    rt_snprintf(cmd, sizeof(cmd), "%s %s",
                agent_audio_cmd[opt.cmd],
                agent_audio_option[opt.option]);
    return msh_exec(cmd, rt_strlen(cmd));
}

int agent_driver_audio_set_volume(int vol)
{
    char cmd[64] = {0};
    vol = vol < 0 ? 0 : (vol > 99 ? 99 : vol);
    rt_snprintf(cmd, sizeof(cmd), "wavplay -v %d", vol);
    LOG_I("set volume:%d", vol);
    return msh_exec(cmd, rt_strlen(cmd));
}

int agent_driver_audio_record(const char* path)
{
    if (path == RT_NULL || rt_strlen(path) == 0)
    {
        LOG_E("record path is NULL");
        return -RT_EINVAL;
    }

    record_thread_ctx_t *ctx = rt_malloc(sizeof(record_thread_ctx_t));
    if(ctx == RT_NULL)
    {
        LOG_E("malloc record ctx failed");
        return -RT_ENOMEM;
    }
    rt_memset(ctx, 0, sizeof(*ctx));

    AgentAudio opt = {.cmd = AGENT_WAV_RECORD, .option = AGENT_WAV_START};
    rt_snprintf(ctx->cmd_buf, sizeof(ctx->cmd_buf),
        "%s %s %s %d %d %d",
        agent_audio_cmd[opt.cmd],
        agent_audio_option[opt.option],
        path,
        REC_SAMPLE_RATE,
        REC_CHANNELS,
        REC_SAMPLE_BITS);

    LOG_I("record run cmd: [%s]", ctx->cmd_buf);

    rt_thread_t tid = rt_thread_create("rec_work",
                                        record_worker_entry,
                                        ctx,
                                        REC_THREAD_STACK,
                                        REC_THREAD_PRIO,
                                        20);
    if(tid == RT_NULL)
    {
        LOG_E("create record thread fail");
        rt_free(ctx);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}

int agent_driver_audio_record_stop(void)
{
    char cmd[64] = {0};
    AgentAudio opt = {.cmd = AGENT_WAV_RECORD, .option = AGENT_WAV_STOP};
    rt_snprintf(cmd, sizeof(cmd), "%s %s",
        agent_audio_cmd[opt.cmd],
        agent_audio_option[opt.option]);

    LOG_I("record stop cmd: [%s]", cmd);
    return msh_exec(cmd, rt_strlen(cmd));
}