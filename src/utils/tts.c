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
 * @brief 拼出 TTS 接口地址
 * @param buf  存放地址的缓冲区
 * @param size 缓冲区大小
 * @return RT_EOK 成功；地址为空或放不下返回负值
 */
static rt_err_t tts_url(char *buf, size_t size)
{
    const char *base = agent_endpoint_file_server();

    if (base[0] == '\0')
    {
        LOG_E("[tts] file server address is empty");
        return -RT_EINVAL;
    }
    int n = rt_snprintf(buf, size, "%s/tts", base);
    if (n < 0 || n >= (int)size)
    {
        LOG_E("[tts] url too long");
        return -RT_EINVAL;
    }
    return RT_EOK;
}

/*
 * @brief 建会话、POST 合成请求并等到响应头，返回可以直接收响应体的会话
 * @param text   待合成文本（UTF-8，非空）
 * @param url    TTS 接口地址
 * @param stream 非 0 时要求服务端流式分块，响应体变成 [u32 长度][WAV] 的帧序列
 * @return 会话指针，调用方负责 webclient_close()；失败返回 RT_NULL
 * @note 照 STT 的老办法：先只发请求头，连上之后把超时放宽，再自己写请求体、
 *       自己收响应，避免服务端首帧几十秒把默认超时撞穿。
 */
static struct webclient_session *tts_post(const char *text, rt_bool_t stream, const char *url)
{
    struct webclient_session *session = RT_NULL;
    cJSON *root = RT_NULL;
    char *body = RT_NULL;
    rt_off_t body_len;
    int resp_code;

    root = cJSON_CreateObject();
    if (root == RT_NULL) return RT_NULL;
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddNumberToObject(root, "sample_rate", AGENT_TTS_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", AGENT_TTS_CHANNELS);
    if (stream)
    {
        cJSON_AddItemToObject(root, "stream", cJSON_CreateTrue());
        cJSON_AddNumberToObject(root, "block_seconds", (double)AGENT_TTS_BLOCK_SECONDS);
    }
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == RT_NULL) return RT_NULL;
    body_len = (rt_off_t)rt_strlen(body);

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
    {
        LOG_W("[tts] session create failed (out of memory)");
        cJSON_free(body);
        return RT_NULL;
    }
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", (int)body_len);

    LOG_I("[tts] POST %s, text %d bytes%s", url, (int)body_len,
          stream ? " (stream)" : "");
    if (webclient_post(session, url, RT_NULL, 0) < 0)
    {
        LOG_W("[tts] post connect fail");
        goto fail;
    }
    agent_webclient_set_timeout(session, AGENT_TTS_TIMEOUT_MS);
    if (webclient_write(session, body, (size_t)body_len) != (int)body_len)
    {
        LOG_W("[tts] send body fail");
        goto fail;
    }

    resp_code = webclient_handle_response(session);
    if (resp_code != 200)
    {
        LOG_W("[tts] http resp code %d", resp_code);
        goto fail;
    }
    cJSON_free(body);
    return session;

fail:
    webclient_close(session);
    cJSON_free(body);
    return RT_NULL;
}

/*
 * @brief 从响应体读满 need 字节
 * @return 实际读到的字节数；小于 need 且不小于 0 表示服务端把流发完了；负值是收发错误
 * @note webclient_read() 遇到 HTTP chunk 边界会提前返回，所以帧头这种小计数
 *       必须自己循环拼，不能假设一次就读满
 */
static int tts_read_exact(struct webclient_session *session, unsigned char *buf, int need)
{
    int got = 0, len;

    while (got < need)
    {
        len = webclient_read(session, buf + got, need - got);
        if (len <= 0) return got ? got : len;
        got += len;
    }
    return got;
}

/*
 * @brief 收一整帧 [u32 长度][WAV] 写进 fd
 * @return 帧内字节数；0 表示流正常结束；负值表示中断或写盘失败
 */
static int tts_read_frame_to_fd(struct webclient_session *session, int fd,
                                unsigned char *buf, int cap)
{
    rt_uint32_t len;
    int total = 0, got, want, hdr;

    hdr = tts_read_exact(session, buf, 4);
    if (hdr < 0)
    {
        LOG_W("[tts] read frame header failed (%d)", hdr);
        return hdr;
    }
    if (hdr < 4)
    {
        return 0;
    }
    len = (rt_uint32_t)buf[0] | ((rt_uint32_t)buf[1] << 8) |
          ((rt_uint32_t)buf[2] << 16) | ((rt_uint32_t)buf[3] << 24);
    /* 帧长超出合理范围只会是错位的字节，继续读会把整条流读歪，直接判失败 */
    if (len < 44 || len > 4u * 1024u * 1024u)
    {
        LOG_W("[tts] bad frame length %u", (unsigned int)len);
        return -RT_ERROR;
    }

    while (total < (int)len)
    {
        want = (int)len - total;
        if (want > cap) want = cap;
        got = webclient_read(session, buf, want);
        if (got < 0)
        {
            LOG_W("[tts] read frame body failed at %d/%u bytes (%d)", total,
                  (unsigned int)len, got);
            return got;
        }
        if (got == 0)
        {
            LOG_W("[tts] frame cut short, got %d/%u bytes", total, (unsigned int)len);
            return -RT_ERROR;
        }
        if (write(fd, buf, got) != got)
        {
            LOG_W("[tts] write frame fail at %d", total);
            return -RT_ERROR;
        }
        total += got;
    }
    return total;
}

/*
 * @brief 把文本交给 TTS 接口合成，响应体（audio/wav）边收边写进本地文件
 *
 * 方向与 agent_stt_transcribe 相反：请求体是 JSON，响应体是二进制 WAV。
 * 所以不能走 webclient_response()，那会把整个 WAV 一次性读进内存；这里照
 * webclient_get_file 的做法，按 Content-Length 分块 webclient_read 后落盘。
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
    unsigned char *io_buf = RT_NULL;
    struct webclient_session *session = RT_NULL;
    char url[AGENT_TTS_URL_BUF_SZ] = {0};
    int clen = 0;

    if (text == RT_NULL || text[0] == '\0' || save_path == RT_NULL)
    {
        return -RT_EINVAL;
    }
    if (tts_url(url, sizeof(url)) != RT_EOK)
    {
        return -RT_EINVAL;
    }

    session = tts_post(text, RT_FALSE, url);
    if (session == RT_NULL)
    {
        return -RT_ERROR;
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

    if (clen > 0 && total < clen)
    {
        /* 收一半断了：这份 WAV 头部完整但数据短，播起来就是"说一句就停"，
           必须算失败，不能让调用方以为拿到整条音频 */
        LOG_W("[tts] stream ended early, got %d/%d bytes", total, clen);
        goto out;
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
    webclient_close(session);
    if (io_buf) rt_free(io_buf);
    if (fd >= 0) close(fd);
    return rc;
}

/*
 * @brief TTS 命令行入口
 * @usage
 *   msh_agent_tts <文本>            合成到 AGENT_TTS_SAVE_PATH 并播放
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
        {
            char url[AGENT_TTS_URL_BUF_SZ] = {0};
            tts_url(url, sizeof(url));
            rt_kprintf("  server: %s, default save: %s\n", url, AGENT_TTS_SAVE_PATH);
        }
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
 * 回复自动播报。agent_tts_speak() 只把整条回复的文本指针投进队列，收流与播放都在
 * 工作线程里，免得服务端几十秒的推流把主循环卡住，顺带挡住后面的消息。
 * 一条回复只发一次请求，服务端按 [u32 长度][WAV] 一帧一帧吐，收到一帧就能播一帧：
 * 第一帧只有 AGENT_TTS_BLOCK_SECONDS 秒音频，1 s 出头就起播，而不是等整条合成完。
 * 落盘还是两个槽乒乓，收下一帧和播当前这一帧并行。
 */
#ifdef PKG_AGENT_TTS_AUTO_PLAY

static rt_mq_t     g_tts_speak_mq     = RT_NULL;
static rt_thread_t g_tts_speak_thread = RT_NULL;

/*
 * @brief 算出第 slot 个 WAV 落盘路径（在扩展名前插一位序号）
 * @note  合成写空闲槽、播放切到刚写好的槽，正播的文件不会被 O_TRUNC 抢，
 *        于是合成与下载能和上一句的播放并行
 */
static void tts_slot_path(int slot, char *out, rt_size_t size)
{
    const char *base = AGENT_TTS_SAVE_PATH;
    rt_size_t len = rt_strlen(base);
    rt_size_t dot = len;
    int i;

    for (i = (int)len - 1; i >= 0; i--)
    {
        if (base[i] == '.') { dot = (rt_size_t)i; break; }
    }
    /* 序号 + 点 + 扩展名至少留 8 字节，放不下就退回在末尾追加序号 */
    if (dot + 8 >= size) dot = len;

    rt_snprintf(out, size, "%.*s%d%s", (int)dot, base, slot & 1, base + dot);
}

/*
 * @brief 播一条回复：请求服务端流式合成，收一帧播一帧
 * @note  收下一帧和播当前这一帧是重叠的（空闲槽收新帧，正播的槽不动），
 *        所以只有服务端真的跟不上时才会听见停顿
 */
static void tts_play_stream(const char *text)
{
    struct webclient_session *session;
    unsigned char *io_buf = RT_NULL;
    char path[AGENT_TTS_SLOT_PATH_SZ];
    char url[AGENT_TTS_URL_BUF_SZ] = {0};
    int slot = 0, blocks = 0, frame;
    rt_bool_t broke = RT_FALSE;

    /* 上一条回复的最后一帧可能还在播，先等它放完：下面 O_TRUNC 会截掉正在读的文件 */
    if (agent_driver_audio_play_wait(AGENT_TTS_PLAY_WAIT_MS) != RT_EOK)
    {
        agent_driver_audio_stop();
    }

    if (tts_url(url, sizeof(url)) != RT_EOK)
    {
        return;
    }

    session = tts_post(text, RT_TRUE, url);
    if (session == RT_NULL)
    {
        LOG_W("[tts] reply stream request failed");
        return;
    }
    io_buf = (unsigned char *)rt_malloc(AGENT_TTS_IO_BUF_SZ);
    if (io_buf == RT_NULL)
    {
        webclient_close(session);
        return;
    }

    while (blocks < AGENT_TTS_MAX_BLOCKS)
    {
        int fd;

        tts_slot_path(slot, path, sizeof(path));
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
        if (fd < 0)
        {
            LOG_W("[tts] open %s fail", path);
            broke = RT_TRUE;
            break;
        }
        frame = tts_read_frame_to_fd(session, fd, io_buf, AGENT_TTS_IO_BUF_SZ);
        close(fd);
        if (frame < 0)
        {
            /* 半帧播出去就是戛然而止，丢掉这帧收尾 */
            broke = RT_TRUE;
            break;
        }
        if (frame == 0)
        {
            break;      /* 服务端发完了 */
        }

        if (agent_driver_audio_play_wait(AGENT_TTS_PLAY_WAIT_MS) != RT_EOK)
        {
            LOG_W("[tts] previous block still playing, stop it first");
            agent_driver_audio_stop();
        }
        if (agent_driver_audio_play(path) != RT_EOK)
        {
            LOG_W("[tts] block %d playback failed", blocks);
            broke = RT_TRUE;
            break;
        }
        slot ^= 1;
        blocks++;
    }

    if (blocks == 0)
    {
        LOG_W("[tts] no audio block from server");
    }
    else if (broke)
    {
        LOG_W("[tts] stream broke after %d blocks", blocks);
    }
    else if (blocks >= AGENT_TTS_MAX_BLOCKS)
    {
        LOG_W("[tts] reply longer than %d blocks, dropped the tail", AGENT_TTS_MAX_BLOCKS);
    }
    else
    {
        LOG_I("[tts] reply played in %d blocks", blocks);
    }

    rt_free(io_buf);
    webclient_close(session);
}

/*
 * @brief  播报线程入口，从队列取文本逐条合成并播放
 * @param  arg 未使用
 */
static void tts_speak_work_thread(void *arg)
{
    char *text = RT_NULL;

    /* rt_mq_recv() 返回收到的字节数而不是 RT_EOK，<=0 表示队列被删或参数不匹配 */
    while (rt_mq_recv(g_tts_speak_mq, &text, sizeof(text), RT_WAITING_FOREVER) > 0)
    {
        if (text == RT_NULL) continue;
        tts_play_stream(text);
        rt_free(text);
    }
    LOG_E("[tts] speak worker stopped");
}

/*
 * @brief 创建播报队列与工作线程，由 init_agent() 调用
 */
void agent_tts_speak_init(void)
{
    char path[AGENT_TTS_SLOT_PATH_SZ], alt[AGENT_TTS_SLOT_PATH_SZ];

    g_tts_speak_mq = rt_mq_create("tts_spk_mq", sizeof(char *),
                                  AGENT_TTS_SPEAK_QUEUE, RT_IPC_FLAG_FIFO);
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
    tts_slot_path(0, path, sizeof(path));
    tts_slot_path(1, alt, sizeof(alt));
    {
        char url[AGENT_TTS_URL_BUF_SZ] = {0};
        tts_url(url, sizeof(url));
        LOG_I("[tts] reply auto play on, server=%s slots=%s %s", url, path, alt);
    }
}

/*
 * @brief 把整条回复投进播报队列，不等合成
 * @param text UTF-8 文本
 * @note  队列按定长条目搬运，装不下几千字节的回复，所以这里拷一份到堆上、
 *        只把指针入队，工作线程播完再释放
 */
void agent_tts_speak(const char *text)
{
    rt_size_t len;
    char *copy;

    if (g_tts_speak_mq == RT_NULL || text == RT_NULL || text[0] == '\0')
    {
        return;
    }

    len = rt_strlen(text);
    copy = (char *)rt_malloc(len + 1);
    if (copy == RT_NULL)
    {
        LOG_W("[tts] no memory for a %d-byte reply", (int)len);
        return;
    }
    rt_memcpy(copy, text, len + 1);

    if (rt_mq_send(g_tts_speak_mq, &copy, sizeof(copy)) != RT_EOK)
    {
        LOG_W("[tts] speak queue full, reply not announced");
        rt_free(copy);
    }
}
#endif /* PKG_AGENT_TTS_AUTO_PLAY */

#endif /* PKG_AGENT_TTS_ENABLE */
