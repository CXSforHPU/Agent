#include "utils/utils_internal.h"
#include "dfs_posix.h"
#include "unistd.h"

#define LOG_TAG "Agent.utils.stt"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#ifdef PKG_AGENT_STT_ENABLE

/*
 * @brief 把本地 WAV 通过 STT 接口转成文本（multipart POST，回 JSON）
 *
 * 与 agent_post_file_stream 类似，区别是不取回 URL，而是把响应体里的
 * {"code":0,"data":{"text":"..."}} 里的 text 解析出来返回。返回的指针指向
 * 静态缓冲区，调用者需及时拷贝。
 *
 * @param path 本地 WAV 文件路径
 * @return 转写后的文本；NULL 表示失败
 */
const char *agent_stt_transcribe(const char *path)
{
    static char result_text[1024];
    int fd = -1, rc = WEBCLIENT_OK;
    unsigned char *io_buf = RT_NULL;
    struct webclient_session *session = RT_NULL;
    int wlen, rlen, total = 0;
    int resp_code = 0;

    result_text[0] = '\0';

    if (path == RT_NULL)
    {
        return RT_NULL;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        LOG_W("[stt] open %s fail", path);
        return RT_NULL;
    }
    rt_off_t file_sz = lseek(fd, 0, SEEK_END);
    if (file_sz < 0)
    {
        close(fd);
        return RT_NULL;
    }
    lseek(fd, 0, SEEK_SET);

    io_buf = (unsigned char *)rt_malloc(AGENT_STT_IO_BUF_SZ);
    if (io_buf == RT_NULL) { close(fd); return RT_NULL; }

    char boundary[60];
    rt_snprintf(boundary, sizeof(boundary), "----------------------------%012d", rt_tick_get());
    const char *uri = PKG_AGENT_STT_URL;
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    char form_data[128];
    rt_snprintf(form_data, sizeof(form_data),
                "name=\"file\"; filename=\"%s\"", filename);
    size_t prefix_len = rt_snprintf(RT_NULL, 0,
            "--%s\r\nContent-Disposition: form-data; %s\r\n"
            "Content-Type: audio/wav\r\n\r\n",
            boundary, form_data);
    size_t suffix_len = rt_snprintf(RT_NULL, 0, "\r\n--%s--\r\n", boundary);
    size_t total_length = prefix_len + (size_t)file_sz + suffix_len;

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL) { rt_free(io_buf); close(fd); return RT_NULL; }

    webclient_header_fields_add(session, "Content-Length: %zu\r\n", total_length);
    webclient_header_fields_add(session,
        "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);

    LOG_I("[stt] POST %s, file=%s (%d bytes)", uri, filename, (int)file_sz);
    rc = webclient_post(session, uri, RT_NULL, 0);
    if (rc < 0) { LOG_W("[stt] post connect fail %d", rc); goto out; }

    /* 首次识别要加载 whisper 模型，可能耗时数十秒；socket 已连上后再把
       收发超时放宽到 120s（见 agent_post_file_stream 中的说明）。 */
    webclient_set_timeout(session, AGENT_STT_TIMEOUT_MS);

    /* prefix */
    wlen = rt_snprintf((char *)io_buf, AGENT_STT_IO_BUF_SZ,
            "--%s\r\nContent-Disposition: form-data; %s\r\n"
            "Content-Type: audio/wav\r\n\r\n",
            boundary, form_data);
    if (webclient_write(session, io_buf, wlen) != wlen) { rc = -1; goto out; }

    /* file body */
    while ((rlen = read(fd, io_buf, AGENT_STT_IO_BUF_SZ)) > 0)
    {
        if (webclient_write(session, io_buf, rlen) != rlen) { rc = -1; goto out; }
    }

    /* suffix */
    wlen = rt_snprintf((char *)io_buf, AGENT_STT_IO_BUF_SZ, "\r\n--%s--\r\n", boundary);
    if (webclient_write(session, io_buf, wlen) != wlen) { rc = -1; goto out; }

    extern int webclient_handle_response(struct webclient_session *session);
    resp_code = webclient_handle_response(session);
    if (resp_code != 200)
    {
        LOG_W("[stt] http resp code %d", resp_code);
        rc = -1;
        goto out;
    }
    int clen = (int)webclient_content_length_get(session);
    int want = (clen > 0 && clen < (int)sizeof(result_text) - 1) ? clen
              : (int)sizeof(result_text) - 1;
    while (total < want)
    {
        rlen = webclient_read(session, (unsigned char *)result_text + total, want - total);
        if (rlen <= 0) break;
        total += rlen;
    }
    result_text[total] = '\0';
    {
        cJSON *root = cJSON_Parse(result_text);
        if (root)
        {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *txt = data ? cJSON_GetObjectItem(data, "text") : RT_NULL;
            if (txt && cJSON_IsString(txt) && txt->valuestring)
            {
                rt_strncpy(result_text, txt->valuestring, sizeof(result_text) - 1);
                result_text[sizeof(result_text) - 1] = '\0';
            }
            cJSON_Delete(root);
        }
    }

out:
    if (rc < 0) result_text[0] = '\0';
    if (session) webclient_close(session);
    if (io_buf) rt_free(io_buf);
    if (fd >= 0) close(fd);
    return result_text[0] ? result_text : RT_NULL;
}

#endif /* PKG_AGENT_STT_ENABLE */
