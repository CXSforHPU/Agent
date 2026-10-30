/* 文件服务器 - 多模态 */
#include "utils/utils_internal.h"
#include "dfs_posix.h"
#include "unistd.h"

#define LOG_TAG "Agent.utils.file_op"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#ifdef PKG_AGENT_MULTIMODAL_ENABLE

/* ==================== 静态全局变量 ==================== */
static rt_mq_t g_file_op_mq_input = RT_NULL;
static rt_mq_t g_file_op_mq_output = RT_NULL;
static rt_thread_t g_file_op_thread = RT_NULL;
static rt_sem_t g_exit_sem = RT_NULL;
/*
 * @brief 从路径中提取文件名
 */
static rt_err_t path_get_filename(const char *path, char *out_name, size_t out_len)
{
    const char *p;
    if (!path || !out_name || out_len == 0)
        return -RT_EINVAL;
    rt_memset(out_name, 0, out_len);
    p = strrchr(path, '/');
    if (p != RT_NULL)
        p++;
    else
        p = path;
    rt_strncpy(out_name, p, out_len - 1);
    out_name[out_len - 1] = '\0';
    return RT_EOK;
}

/*
 * @brief 流式上传文件到文件服务器
 */
static int agent_post_file_stream(const char *URI, const char *filename,
                                  const char *form_data, char *out_resp_buf, size_t out_resp_len)
{
    rt_off_t file_sz;
    size_t total_length;
    char boundary[60];
    int fd = -1, rc = WEBCLIENT_OK;
    unsigned char *io_buf = RT_NULL;
    struct webclient_session *session = RT_NULL;
    int wlen, rlen, bytes_read;
    int resp_code, resp_data_len;

    if (!URI || !filename || !form_data)
        return -WEBCLIENT_FILE_ERROR;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        LOG_D("open file %s fail", filename);
        return -WEBCLIENT_FILE_ERROR;
    }
    file_sz = lseek(fd, 0, SEEK_END);
    if (file_sz < 0)
    {
        rc = -WEBCLIENT_FILE_ERROR;
        goto cleanup;
    }
    lseek(fd, 0, SEEK_SET);

    io_buf = (unsigned char *)rt_malloc(AGENT_FILE_OP_IO_BUF_SZ);
    if (io_buf == RT_NULL)
    {
        rc = -WEBCLIENT_NOMEM;
        goto cleanup;
    }

    rt_snprintf(boundary, sizeof(boundary), "----------------------------%012d", rt_tick_get());

    size_t prefix_len = rt_snprintf(RT_NULL, 0,
            "--%s\r\n"
            "Content-Disposition: form-data; %s\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            boundary, form_data);
    size_t suffix_len = rt_snprintf(RT_NULL, 0, "\r\n--%s--\r\n", boundary);
    total_length = prefix_len + (size_t)file_sz + suffix_len;

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
    {
        rc = -WEBCLIENT_NOMEM;
        goto cleanup;
    }
    webclient_header_fields_add(session, "Content-Length: %zu\r\n", total_length);
    webclient_header_fields_add(session, "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);

    rc = webclient_post(session, URI, NULL, 0);
    if (rc < 0)
    {
        LOG_D("webclient_post stream fail %d", rc);
        goto cleanup;
    }

    /* webclient_set_timeout() 作用在 session->socket 上，必须在 connect
       成功之后调用；之前放在 post 前时 socket 还是 -1，设置不生效，实际仍是
       webclient 默认的 6s 收发超时，大文件或慢响应会中途超时。 */
    webclient_set_timeout(session, AGENT_FILE_OP_TIMEOUT_MS);

    /* 1. 发送 multipart 头部 */
    wlen = rt_snprintf((char *)io_buf, AGENT_FILE_OP_IO_BUF_SZ,
            "--%s\r\n"
            "Content-Disposition: form-data; %s\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            boundary, form_data);
    if (webclient_write(session, io_buf, wlen) != wlen)
    {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 2. 分片发送文件内容 */
    while ((rlen = read(fd, io_buf, AGENT_FILE_OP_IO_BUF_SZ)) > 0)
    {
        if (webclient_write(session, io_buf, rlen) != rlen)
        {
            rc = -WEBCLIENT_ERROR;
            goto cleanup;
        }
    }

    /* 3. 发送尾部边界 */
    wlen = rt_snprintf((char *)io_buf, AGENT_FILE_OP_IO_BUF_SZ, "\r\n--%s--\r\n", boundary);
    if (webclient_write(session, io_buf, wlen) != wlen)
    {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 处理响应 */
    extern int webclient_handle_response(struct webclient_session *session);
    if (webclient_handle_response(session) != 200)
    {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }
    resp_code = webclient_resp_status_get(session);
    if (resp_code < 200 || resp_code >= 300)
    {
        LOG_D("upload http code:%d", resp_code);
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 读取响应体 */
    resp_data_len = webclient_content_length_get(session);
    if (resp_data_len > 0 && out_resp_buf && out_resp_len > 0)
    {
        int copied = 0;
        rt_memset(out_resp_buf, 0, out_resp_len);
        while (resp_data_len > 0)
        {
            int to_read = (resp_data_len < AGENT_FILE_OP_IO_BUF_SZ) ? resp_data_len : AGENT_FILE_OP_IO_BUF_SZ;
            bytes_read = webclient_read(session, io_buf, to_read);
            if (bytes_read <= 0) break;
            int copy_now = (copied + bytes_read < (int)out_resp_len - 1) ? bytes_read : (int)(out_resp_len - 1 - copied);
            if (copy_now > 0)
            {
                rt_memcpy(out_resp_buf + copied, io_buf, copy_now);
                copied += copy_now;
            }
            resp_data_len -= bytes_read;
        }
    }
    else
    {
        while (resp_data_len > 0)
        {
            int to_read = (resp_data_len < AGENT_FILE_OP_IO_BUF_SZ) ? resp_data_len : AGENT_FILE_OP_IO_BUF_SZ;
            if (webclient_read(session, io_buf, to_read) <= 0) break;
            resp_data_len -= to_read;
        }
    }
    rc = WEBCLIENT_OK;

cleanup:
    if (fd >= 0) close(fd);
    if (session) webclient_close(session);
    if (io_buf) rt_free(io_buf);
    return rc;
}

/*
 * @brief 根据 file_id 下载文件
 */
static rt_err_t agent_multi_get_file(const char *file_id, const char *path)
{
    char url[128] = {0};
    rt_snprintf(url, sizeof(url), "%s/%s", PKG_AGENT_DOWNLOAD_URL, file_id);
    LOG_I("[download] url: %s -> %s", url, path);
    int ret = webclient_get_file(url, path);
    if (ret == 0)
    {
        LOG_I("[download] success");
        return RT_EOK;
    }
    else
    {
        LOG_E("[download] failed %d", ret);
        return -RT_ERROR;
    }
}

/*
 * @brief 上传文件封装（解析 JSON 返回 URL）
 */
static rt_err_t agent_upload_file(const char *path, char *out_url, size_t out_url_len)
{
    char resp_buf[512] = {0};
    char form_data[128] = {0};
    char filename[64] = {0};
    cJSON *resp_json = RT_NULL;
    rt_err_t ret = -RT_ERROR;

    if (!path || !out_url || out_url_len == 0)
        return -RT_EINVAL;
    rt_memset(out_url, 0, out_url_len);

    path_get_filename(path, filename, sizeof(filename));
    rt_snprintf(form_data, sizeof(form_data), "name=\"file\";filename=\"%s\"", filename);

    int upload_ret = agent_post_file_stream(PKG_AGENT_UPLOAD_URL, path, form_data,
                                            resp_buf, sizeof(resp_buf));
    if (upload_ret != WEBCLIENT_OK)
    {
        LOG_E("upload file failed, err=%d", upload_ret);
        return -RT_ERROR;
    }

    LOG_I("upload success, rsp: %s", resp_buf);

    resp_json = cJSON_Parse(resp_buf);
    if (resp_json == RT_NULL)
    {
        LOG_E("json parse fail");
        goto exit_fail;
    }

    cJSON *data = cJSON_GetObjectItem(resp_json, "data");
    if (!data)
    {
        LOG_E("no data field");
        goto exit_fail;
    }
    cJSON *file_id_json = cJSON_GetObjectItem(data, "file_id");
    if (!file_id_json || !cJSON_IsString(file_id_json) || !file_id_json->valuestring)
    {
        LOG_E("no file_id field");
        goto exit_fail;
    }

    int sn = rt_snprintf(out_url, out_url_len, "%s/%s",
                         PKG_AGENT_DOWNLOAD_URL, file_id_json->valuestring);
    if (sn < 0 || sn >= (int)out_url_len)
    {
        LOG_E("url buffer overflow");
        goto exit_fail;
    }

    ret = RT_EOK;
exit_fail:
    if (resp_json) cJSON_Delete(resp_json);
    return ret;
}

/*
 * @brief 文件操作工作线程
 */
static void file_operation_work_thread(void *arg)
{
    file_op_req req, req_out;
    char url[128] = {0};
    rt_err_t ret;

    while (1)
    {
        rt_memset(&req, 0, sizeof(req));
        rt_mq_recv(g_file_op_mq_input, &req, sizeof(req), RT_WAITING_FOREVER);

        if (req.op == CMD_AGENT_FILE_EXIT)
        {
            LOG_I("[fileop] exit thread");
            rt_sem_release(g_exit_sem);
            break;
        }

        switch (req.op)
        {
            case CMD_AGENT_FILE_UPLOAD:
            {
                rt_kprintf("[upload] start %s\n", req.path);
                ret = agent_upload_file(req.path, url, sizeof(url));
                rt_kprintf("[upload] %s, url=%s\n", (ret == RT_EOK) ? "ok" : "failed", url);

                rt_memset(&req_out, 0, sizeof(req_out));
                req_out.op = CMD_AGENT_FILE_URL;
                const char *p = strrchr(url, '/');
                if (p) p++;
                else p = url;
                rt_strncpy(req_out.file_id, p, sizeof(req_out.file_id) - 1);
                req_out.file_id[sizeof(req_out.file_id) - 1] = '\0';
                rt_strncpy(req_out.path, url, sizeof(req_out.path) - 1);
                req_out.path[sizeof(req_out.path) - 1] = '\0';

                rt_mq_send(g_file_op_mq_output, &req_out, sizeof(req_out));
                break;
            }

            case CMD_AGENT_FILE_DOWNLOAD:
            {
                rt_kprintf("[download] file_id=%s -> %s\n", req.file_id, req.path);
                ret = agent_multi_get_file(req.file_id, req.path);
                rt_kprintf("[download] %s\n", (ret == RT_EOK) ? "ok" : "failed");
                break;
            }

            default:
                rt_kprintf("[file op] unknown op:%d\n", req.op);
                break;
        }
    }
}

/*
 * @brief 获取输入消息队列
 */
rt_mq_t get_agent_file_op_mq_input(void)
{
    return g_file_op_mq_input;
}

/*
 * @brief 获取输出消息队列
 */
rt_mq_t get_agent_file_op_mq_output(void)
{
    return g_file_op_mq_output;
}

/*
 * @brief 初始化文件操作线程
 * @return 线程句柄
 */
rt_thread_t agent_file_op_init(void)
{
    g_file_op_mq_input = rt_mq_create("fileop_mq_in", sizeof(file_op_req), 4, RT_IPC_FLAG_FIFO);
    if (g_file_op_mq_input == RT_NULL)
        return RT_NULL;

    g_file_op_mq_output = rt_mq_create("fileop_mq_out", sizeof(file_op_req), 4, RT_IPC_FLAG_FIFO);
    if (g_file_op_mq_output == RT_NULL)
    {
        rt_mq_delete(g_file_op_mq_input);
        g_file_op_mq_input = RT_NULL;
        return RT_NULL;
    }

    g_exit_sem = rt_sem_create("fileop_exit", 0, RT_IPC_FLAG_FIFO);
    if (g_exit_sem == RT_NULL)
    {
        rt_mq_delete(g_file_op_mq_input);
        rt_mq_delete(g_file_op_mq_output);
        g_file_op_mq_input = RT_NULL;
        g_file_op_mq_output = RT_NULL;
        return RT_NULL;
    }

    g_file_op_thread = rt_thread_create("fileop_wk",
                                        file_operation_work_thread,
                                        RT_NULL,
                                        PKG_AGENT_FILE_OP_THREAD_SIZE,
                                        9,
                                        20);
    if (g_file_op_thread == RT_NULL)
    {
        rt_mq_delete(g_file_op_mq_input);
        rt_mq_delete(g_file_op_mq_output);
        rt_sem_delete(g_exit_sem);
        g_file_op_mq_input = RT_NULL;
        g_file_op_mq_output = RT_NULL;
        g_exit_sem = RT_NULL;
        return RT_NULL;
    }

    rt_thread_startup(g_file_op_thread);
    return g_file_op_thread;
}

/*
 * @brief 销毁文件操作线程
 */
void agent_file_op_deinit(void)
{
    if (g_file_op_mq_input != RT_NULL)
    {
        file_op_req exit_req;
        exit_req.op = CMD_AGENT_FILE_EXIT;
        rt_mq_send(g_file_op_mq_input, &exit_req, sizeof(file_op_req));
        /* 等待线程退出（线程收到命令后会 release g_exit_sem 并自然退出） */
        rt_sem_take(g_exit_sem, RT_WAITING_FOREVER);
    }

    /* 线程由 rt_thread_create 创建，入口函数返回后空闲线程自动清理，无需 rt_thread_delete */
    g_file_op_thread = RT_NULL;

    if (g_file_op_mq_input != RT_NULL)
    {
        rt_mq_delete(g_file_op_mq_input);
        g_file_op_mq_input = RT_NULL;
    }
    if (g_file_op_mq_output != RT_NULL)
    {
        rt_mq_delete(g_file_op_mq_output);
        g_file_op_mq_output = RT_NULL;
    }
    if (g_exit_sem != RT_NULL)
    {
        rt_sem_delete(g_exit_sem);
        g_exit_sem = RT_NULL;
    }

    LOG_I("[fileop] deinit done");
}

/*
 * @brief 下载文件任务入队
 * @param path    保存路径
 * @param file_id 文件 ID
 * @return RT_EOK 成功
 */
rt_err_t agent_down_load(const char *path, const char *file_id)
{
    if (g_file_op_mq_input == RT_NULL)
    {
        LOG_E("file op mq not init");
        return -RT_ERROR;
    }

    file_op_req req;
    rt_memset(&req, 0, sizeof(req));
    req.op = CMD_AGENT_FILE_DOWNLOAD;
    rt_strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    rt_strncpy(req.file_id, file_id, sizeof(req.file_id) - 1);
    req.file_id[sizeof(req.file_id) - 1] = '\0';

    if (rt_mq_send(g_file_op_mq_input, &req, sizeof(req)) != RT_EOK)
    {
        LOG_E("send download request failed");
        return -RT_ERROR;
    }

    LOG_I("download task enqueued, file_id=%s", file_id);
    return RT_EOK;
}

/*
 * @brief 上传文件（同步等待返回 URL）
 * @param path 文件路径
 * @return URL 字符串指针（静态缓冲区，调用者需及时拷贝）
 */
char *agent_up_load(const char *path)
{
    static char result_url[128];
    rt_memset(result_url, 0, sizeof(result_url));

    if (g_file_op_mq_input == RT_NULL || g_file_op_mq_output == RT_NULL)
    {
        LOG_E("file op mq not init");
        return RT_NULL;
    }

    file_op_req req_input, req_output;
    rt_memset(&req_input, 0, sizeof(req_input));
    rt_memset(&req_output, 0, sizeof(req_output));

    req_input.op = CMD_AGENT_FILE_UPLOAD;
    rt_strncpy(req_input.path, path, sizeof(req_input.path) - 1);
    req_input.path[sizeof(req_input.path) - 1] = '\0';

    if (rt_mq_send(g_file_op_mq_input, &req_input, sizeof(req_input)) != RT_EOK)
    {
        LOG_E("send upload request failed");
        return RT_NULL;
    }

    rt_mq_recv(g_file_op_mq_output, &req_output, sizeof(req_output), RT_WAITING_FOREVER);

    rt_strncpy(result_url, req_output.path, sizeof(result_url) - 1);
    result_url[sizeof(result_url) - 1] = '\0';

    LOG_I("upload complete, url=%s", result_url);
    return result_url;
}

/*
 * @brief 文件操作 MSH 命令（合并自 msh_agent_download / msh_agent_upload）
 *
 * 用法：
 *   <cmd> -h | --help | help
 *   <cmd> up   | upload   <file_path>
 *   <cmd> down | download <file_id> <save_path>
 *
 * - 子命令不区分大小写：本实现保留原始 argv 做匹配，调用方按字面传入；
 *   up/down 短名 + upload/download 长名均接受，便于敲命令和脚本拼接。
 * - 帮助触发：argc<2、"-h"、"--help"、"help" 任一命中即打印用法并返回 0。
 * - 未知子命令：打印 error 并提示 "-h"，返回 -1。
 * - MQ 未初始化：所有子命令都会先检查，直接返回 -1，避免后续 send 卡死。
 * @note  入队的任务真正工作由 file_operation_work_thread 完成；本函数只负责
 *        解析参数、打包 file_op_req 并向 g_file_op_mq_input 投递。
 */
static int msh_agent_upload_download(int argc, char *argv[])
{
    rt_mq_t file_op_mq = get_agent_file_op_mq_input();
    const char *prog = (argc > 0 && argv[0] != RT_NULL) ? argv[0] : "upload_download";

    /* 帮助：-h / --help / help / 不带子命令 */
    if (argc < 2 ||
        rt_strcmp(argv[1], "-h")    == 0 ||
        rt_strcmp(argv[1], "--help") == 0 ||
        rt_strcmp(argv[1], "help")   == 0)
    {
        rt_kprintf("Usage:\n");
        rt_kprintf("  %s up   | upload   <file_path>            # upload file to server\n",   prog);
        rt_kprintf("  %s down | download <file_id> <save_path>  # download file from server\n", prog);
        rt_kprintf("  %s -h | --help | help                     # show this help\n",          prog);
        return 0;
    }

    if (file_op_mq == RT_NULL)
    {
        rt_kprintf("error: file op mq not init\n");
        return -1;
    }

    /* 子命令：上传 */
    if (rt_strcmp(argv[1], "up") == 0 || rt_strcmp(argv[1], "upload") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("usage: %s up <file_path>\n", prog);
            return -1;
        }

        file_op_req req;
        rt_memset(&req, 0, sizeof(req));
        req.op = CMD_AGENT_FILE_UPLOAD;
        rt_strncpy(req.path, argv[2], sizeof(req.path) - 1);
        req.path[sizeof(req.path) - 1] = '\0';

        rt_mq_send(file_op_mq, &req, sizeof(req));
        rt_kprintf("up_load task enqueued: %s\n", req.path);
        return 0;
    }

    /* 子命令：下载 */
    if (rt_strcmp(argv[1], "down") == 0 || rt_strcmp(argv[1], "download") == 0)
    {
        if (argc < 4)
        {
            rt_kprintf("usage: %s down <file_id> <save_path>\n", prog);
            return -1;
        }

        file_op_req req;
        rt_memset(&req, 0, sizeof(req));
        req.op = CMD_AGENT_FILE_DOWNLOAD;
        rt_strncpy(req.file_id, argv[2], sizeof(req.file_id) - 1);
        req.file_id[sizeof(req.file_id) - 1] = '\0';
        rt_strncpy(req.path, argv[3], sizeof(req.path) - 1);
        req.path[sizeof(req.path) - 1] = '\0';

        rt_mq_send(file_op_mq, &req, sizeof(req));
        rt_kprintf("down_load task enqueued: %s -> %s\n", req.file_id, req.path);
        return 0;
    }

    /* 未知子命令 */
    rt_kprintf("error: unknown subcommand '%s'\n", argv[1]);
    rt_kprintf("try '%s -h' for help\n", prog);
    return -1;
}

MSH_CMD_EXPORT(msh_agent_upload_download, upload/download file by file_server);

#endif /* PKG_AGENT_MULTIMODAL_ENABLE */
