#include "utils/utils_internal.h"
#include "dfs_posix.h"
#include "unistd.h"

#ifdef PKG_AGENT_DRIVER_AUDIO
#include "AgentAudio.h"
#endif

#define LOG_TAG "Agent.utils.tts"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#ifdef PKG_AGENT_TTS_ENABLE

/*
 * @brief 把文本交给 TTS 接口合成，响应体（audio/wav）边收边写进本地文件
 *
 * 方向与 agent_stt_transcribe 相反：请求体是 JSON，响应体是二进制 WAV。
 * 所以不能走 webclient_response()，那会把整个 WAV 一次性读进内存；这里照
 * webclient_get_file 的做法，按 Content-Length 分块 webclient_read 后落盘。
 * 也照 STT 的老办法：先只发请求头，连上之后把超时放宽，再自己写请求体、
 * 自己收响应，避免服务端首帧几十秒把默认超时撞穿。
 *
 * @param text 待合成文本（UTF-8，非空）
 * @param save_path 本地 WAV 落盘路径
 * @param out_len 落地字节数，可为 NULL
 * @return RT_EOK 成功；负值失败
 */
static rt_err_t agent_tts_synth(const char *text, const char *save_path, rt_off_t *out_len)
{
    rt_err_t rc = -RT_ERROR;
    int fd = -1, len = 0, total = 0;
    char *body = RT_NULL;
    unsigned char *io_buf = RT_NULL;
    cJSON *root = RT_NULL;
    struct webclient_session *session = RT_NULL;
    int resp_code = 0, clen = 0;
    rt_off_t body_len = 0;

    if (text == RT_NULL || text[0] == '\0' || save_path == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (PKG_AGENT_TTS_URL[0] == '\0')
    {
        LOG_E("[tts] PKG_AGENT_TTS_URL is empty");
        return -RT_EINVAL;
    }

    root = cJSON_CreateObject();
    if (root == RT_NULL) return -RT_ENOMEM;
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddNumberToObject(root, "sample_rate", AGENT_TTS_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", AGENT_TTS_CHANNELS);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == RT_NULL) return -RT_ENOMEM;
    body_len = (rt_off_t)rt_strlen(body);

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
    {
        LOG_W("[tts] session create failed (out of memory)");
        rc = -RT_ENOMEM;
        goto out;
    }
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", (int)body_len);

    LOG_I("[tts] POST %s, text %d bytes", PKG_AGENT_TTS_URL, (int)body_len);
    if (webclient_post(session, PKG_AGENT_TTS_URL, RT_NULL, 0) < 0)
    {
        LOG_W("[tts] post connect fail");
        goto out;
    }
    webclient_set_timeout(session, AGENT_TTS_TIMEOUT_MS);
    if (webclient_write(session, body, (size_t)body_len) != (int)body_len)
    {
        LOG_W("[tts] send body fail");
        goto out;
    }

    extern int webclient_handle_response(struct webclient_session *session);
    resp_code = webclient_handle_response(session);
    if (resp_code != 200)
    {
        LOG_W("[tts] http resp code %d", resp_code);
        goto out;
    }

    io_buf = (unsigned char *)rt_malloc(AGENT_TTS_IO_BUF_SZ);
    if (io_buf == RT_NULL) { rc = -RT_ENOMEM; goto out; }
    fd = open(save_path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
    {
        LOG_W("[tts] open %s fail", save_path);
        goto out;
    }

    clen = (int)webclient_content_length_get(session);
    for (;;)
    {
        size_t want = AGENT_TTS_IO_BUF_SZ;
        /* 服务端一般带 Content-Length；没有就一路读到连接结束 */
        if (clen > 0)
        {
            if (total >= clen) break;
            if ((size_t)(clen - total) < want) want = (size_t)(clen - total);
        }
        len = webclient_read(session, io_buf, want);
        if (len <= 0) break;
        if (write(fd, io_buf, len) != len)
        {
            LOG_W("[tts] write %s fail at %d", save_path, total);
            goto out;
        }
        total += len;
    }

    if (total <= 0)
    {
        LOG_W("[tts] empty wav from server");
        goto out;
    }
    LOG_I("[tts] %s saved, %d bytes", save_path, total);
    if (out_len) *out_len = (rt_off_t)total;
    rc = RT_EOK;

out:
    if (session) webclient_close(session);
    if (io_buf) rt_free(io_buf);
    if (fd >= 0) close(fd);
    if (body) cJSON_free(body);
    return rc;
}

/*
 * @brief TTS 命令行入口
 * @usage
 *   msh_agent_tts <文本>            合成到 /tts.wav 并播放
 *   msh_agent_tts -f <文本>         只合成落盘，不播放（没有音频驱动时用）
 *   msh_agent_tts -o <path> <文本>  指定落盘路径
 * @note  合成过程在当前线程同步等待；播放交给 wavplayer 自己的线程，
 *        所以落盘的 WAV 不会被立刻删除，播完还在。
 */
static int msh_agent_tts(int argc, char *argv[])
{
    char text[512] = {0};
    const char *save_path = AGENT_TTS_SAVE_PATH;
    const char *prog = (argc > 0 && argv[0] != RT_NULL) ? argv[0] : "msh_agent_tts";
    rt_bool_t play = RT_TRUE;
    int i, used = 0;

    if (argc < 2 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "help") == 0)
    {
        rt_kprintf("Usage:\n");
        rt_kprintf("  %s [-f] [-o <path>] <text>   # synthesize text to wav and play it\n", prog);
        rt_kprintf("  %s -f <text>                 # synthesize only, no playback\n", prog);
        rt_kprintf("  server: %s\n", PKG_AGENT_TTS_URL);
        return 0;
    }

    for (i = 1; i < argc; i++)
    {
        if (rt_strcmp(argv[i], "-f") == 0)
        {
            play = RT_FALSE;
        }
        else if (rt_strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            save_path = argv[++i];
        }
        else
        {
            /* 剩余参数拼成一句话，msh 不带引号时会按空格拆开 */
            int n = rt_snprintf(text + used, sizeof(text) - used,
                                used ? " %s" : "%s", argv[i]);
            if (n < 0 || used + n >= (int)sizeof(text))
            {
                rt_kprintf("error: text too long (max %d)\n", (int)sizeof(text) - 1);
                return -1;
            }
            used += n;
        }
    }

    if (text[0] == '\0')
    {
        rt_kprintf("error: empty text\n");
        return -1;
    }

    if (agent_tts_synth(text, save_path, RT_NULL) != RT_EOK)
    {
        rt_kprintf("tts failed: %s\n", save_path);
        return -1;
    }

#ifdef PKG_AGENT_DRIVER_AUDIO
    if (play)
    {
        int ret = agent_driver_audio_play(save_path);
        rt_kprintf("play %s, ret=%d (wavplayer runs in its own thread)\n", save_path, ret);
    }
#else
    if (play) rt_kprintf("warning: PKG_AGENT_DRIVER_AUDIO is off, only synthesized\n");
#endif
    return 0;
}

MSH_CMD_EXPORT(msh_agent_tts, text to speech by file_server and play the wav);

/*
 * 回复自动播报。agent_emit_answer() 只把文本投进队列，合成与播放都在工作线程里，
 * 免得服务端一次几秒的合成把主循环卡住，顺带挡住后面的消息。
 */
#ifdef PKG_AGENT_TTS_AUTO_PLAY

typedef struct
{
    char text[AGENT_TTS_SPEAK_TEXT_SZ];
} tts_speak_req;

static rt_mq_t     g_tts_speak_mq     = RT_NULL;
static rt_thread_t g_tts_speak_thread = RT_NULL;

/*
 * @brief 算出 src 前 max 个字节里最后一个完整 UTF-8 字符的结束位置
 * @note  从字节中间截断会留下半个汉字，服务端解析 JSON 直接失败，整段播报就没了
 */
static rt_size_t tts_utf8_prefix_len(const char *src, rt_size_t max)
{
    rt_size_t i = 0, done = 0;

    while (src[i] != '\0' && i < max)
    {
        unsigned char c = (unsigned char)src[i];
        int n = 1;

        if (c >= 0xF0)      n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;

        if (i + (rt_size_t)n > max) break;
        i += (rt_size_t)n;
        done = i;
    }
    return done;
}

static void tts_speak_work_thread(void *arg)
{
    tts_speak_req req;

    /* rt_mq_recv() 返回收到的字节数而不是 RT_EOK，<=0 表示队列被删或参数不匹配 */
    while (rt_mq_recv(g_tts_speak_mq, &req, sizeof(req), RT_WAITING_FOREVER) > 0)
    {
        if (agent_tts_synth(req.text, AGENT_TTS_SAVE_PATH, RT_NULL) != RT_EOK)
        {
            LOG_W("[tts] reply synth failed, skip playback");
            continue;
        }
        if (agent_driver_audio_play(AGENT_TTS_SAVE_PATH) != RT_EOK)
        {
            LOG_W("[tts] reply playback failed");
        }
    }
    LOG_E("[tts] speak worker stopped");
}

/*
 * @brief 创建播报队列与工作线程，由 init_agent() 调用
 */
void agent_tts_speak_init(void)
{
    g_tts_speak_mq = rt_mq_create("tts_spk_mq", sizeof(tts_speak_req), 2, RT_IPC_FLAG_FIFO);
    if (g_tts_speak_mq == RT_NULL)
    {
        LOG_E("[tts] speak mq create failed");
        return;
    }

    g_tts_speak_thread = rt_thread_create("tts_spk",
                                          tts_speak_work_thread,
                                          RT_NULL,
                                          AGENT_TTS_SPEAK_STACK,
                                          12,
                                          20);
    if (g_tts_speak_thread == RT_NULL)
    {
        rt_mq_delete(g_tts_speak_mq);
        g_tts_speak_mq = RT_NULL;
        LOG_E("[tts] speak thread create failed");
        return;
    }

    rt_thread_startup(g_tts_speak_thread);
    LOG_I("[tts] reply auto play on, server=%s", PKG_AGENT_TTS_URL);
}

/*
 * @brief 把回复文本交给播报线程，不等合成
 * @param text UTF-8 文本，超长部分按字符边界截掉
 */
void agent_tts_speak(const char *text)
{
    tts_speak_req req;
    rt_size_t len;

    if (g_tts_speak_mq == RT_NULL || text == RT_NULL || text[0] == '\0')
    {
        return;
    }

    rt_memset(&req, 0, sizeof(req));
    len = tts_utf8_prefix_len(text, sizeof(req.text) - 1);
    if (len == 0)
    {
        return;
    }
    rt_memcpy(req.text, text, len);

    if (rt_mq_send(g_tts_speak_mq, &req, sizeof(req)) != RT_EOK)
    {
        LOG_W("[tts] speak queue full, reply not spoken");
    }
}
#endif /* PKG_AGENT_TTS_AUTO_PLAY */

#endif /* PKG_AGENT_TTS_ENABLE */
