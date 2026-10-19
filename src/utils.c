#include "utils.h"
#include "AgentRuntime.h"   /* agent_is_running / agent_get_message_hub */
#include "dfs_posix.h"
#include "unistd.h"

#define LOG_TAG "Agent.utils"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#ifdef PKG_AGENT_STT_ENABLE
const char *agent_stt_transcribe(const char *path);
#endif

/* 思考过程是否已开始输出（用于整段连续打印，避免逐片段刷日志） */
static rt_bool_t g_reasoning_started = RT_FALSE;

/*
 * @brief 打印思考过程
 * @note  推理模型的思考内容是逐片段流式返回的（每片段可能只有 1 个字符），
 *        若逐片段调 LOG_I 会把串口刷爆；这里首片段加前缀、其后连续追加，
 *        与正文流式输出保持一致的可读性
 */
void print_reasoning(const char *text)
{
    if (text == RT_NULL)
    {
        return;
    }

    if (!g_reasoning_started)
    {
        rt_kprintf("\n[thinking] ");
        g_reasoning_started = RT_TRUE;
    }
    rt_kprintf("%s", text);
}

/*
 * @brief 打印工具调用信息
 */
void print_tool_call(const char *text)
{
    LOG_I("Tool Call: \n%s", text);
}

/*
 * @brief 打印回复内容
 * @note  正文开始意味着本轮思考结束，复位标记使下一轮思考重新起头
 */
void print_context(const char *text)
{
    g_reasoning_started = RT_FALSE;
    rt_kprintf("%s", text);
}

/*
 * @brief 把一段文本作为用户消息注入 agent（等待 LLM 分析/总结/告警）
 * @param text 文本内容（非空）
 * @return RT_EOK 成功；RT_ERROR agent 未运行、消息中心未就绪或注入失败
 * @note  公共注入入口：供工具、驱动或其它内核模块把外部事件（MQTT 消息、
 *        传感器事件、文件到达等）主动交给 agent 处理，无需自己拼 MessageHub。
 *        - 消息以 role=user 的文本形式进入对话上下文；
 *        - put 成功后消息所有权移交 agent 主循环，调用方不得再释放；
 *        - agent 未运行（未执行 main_loop_entry / 已 cleanup）时返回 RT_ERROR，
 *          调用方应自行缓存或丢弃待注入内容；
 *        - 需要错峰时先判断 agent_is_busy()：agent 正在处理上一轮时投递会
 *          排到该轮结束之后（框架侧不会再合并/丢弃）。
 */
rt_err_t agent_inject_text(const char *text)
{
    MessageHub_t hub;
    Messages_t messages;

    if (text == RT_NULL || text[0] == '\0' || !agent_is_running())
    {
        return RT_ERROR;
    }

    hub = agent_get_message_hub();
    if (hub == RT_NULL)
    {
        return RT_ERROR;
    }

    messages = messages_create(1);
    if (messages == RT_NULL)
    {
        return RT_ERROR;
    }
    if (messages_append(messages, TYPE_TEXT, text) != RT_EOK)
    {
        messages_destroy(messages);
        return RT_ERROR;
    }

    /* 所有权移交：成功后由 agent 主循环消费并释放 */
    if (hub->put_message(hub, messages, hub->input_mailbox) != RT_EOK)
    {
        messages_destroy(messages);
        LOG_W("agent input mailbox full, drop injected message");
        return RT_ERROR;
    }

    return RT_EOK;
}

/*
 * @brief 格式化文本为 cJSON 对象 { type: "text", text: "..." }
 * @param text 文本内容
 * @return cJSON 对象指针
 */
cJSON *format_text(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "type", "text");

    return root;
}

/*
 * @brief 创建空内容数组
 * @return cJSON 数组指针
 */
cJSON *create_content_array(void)
{
    cJSON *content_array = cJSON_CreateArray();
    if (content_array == NULL)
    {
        return NULL;
    }
    return content_array;
}

/*
 * @brief 将 Messages 转换为 API 要求的 content 字段
 * @param messages 消息数组
 * @return 非多模态返回 {content: xxx}，多模态返回 [{type, ...}, ...]
 */
cJSON *to_content(Messages_t messages)
{
    if (!messages)
    {
        return RT_NULL;
    }

#if defined(PKG_AGENT_MULTIMODAL_DISABLE)
    /* 非多模态：返回一个对象，包含 "content" 字段 */

    if (messages_get_content_idx(messages, 0))
    {
        return cJSON_CreateString(messages_get_content_idx(messages,0));
    }

    return RT_NULL;

#elif defined(PKG_AGENT_MULTIMODAL_ENABLE)
    /* 多模态：返回一个数组，每个元素为 { type, ... } */
    cJSON *content_arr = cJSON_CreateArray();
    if (!content_arr)
    {
        return RT_NULL;
    }

    for (int i = 0; i < messages->current_size; i++)
    {
        if (!messages_get_content_idx(messages, i))
        {
            continue;
        }

        switch (messages_get_type_idx(messages, i))
        {
            case TYPE_TEXT:
            {
                cJSON *text = cJSON_CreateObject();
                if (!text) continue;
                cJSON_AddStringToObject(text, "type", get_agent_content_type(TYPE_TEXT));
                cJSON_AddStringToObject(text, get_agent_content_type(TYPE_TEXT), messages_get_content_idx(messages, i));
                cJSON_AddItemToArray(content_arr, text);
                break;
            }

            case TYPE_AUDIO:
            {
#ifdef PKG_AGENT_STT_ENABLE
                const char *audio_path = messages_get_content_idx(messages, i);
                const char *text = agent_stt_transcribe(audio_path);
                /* 录音只是 STT 的临时输入，文本生成后立即删除，避免 SD 卡堆积 */
                unlink(audio_path);
                if (text != RT_NULL && text[0] != '\0')
                {
                    cJSON *t = cJSON_CreateObject();
                    if (t != RT_NULL)
                    {
                        cJSON_AddStringToObject(t, "type",
                            get_agent_content_type(TYPE_TEXT));
                        cJSON_AddStringToObject(t,
                            get_agent_content_type(TYPE_TEXT), text);
                        cJSON_AddItemToArray(content_arr, t);
                        break;
                    }
                }
                LOG_W("[agent] STT failed for %s", audio_path);
                /* DeepSeek 等纯文本模型不接受 audio_url，STT 失败后再回退上传
                   音频会让整个请求 422；这里改为一条提示文本，保证请求送达 */
                cJSON *hint = cJSON_CreateObject();
                if (hint != RT_NULL)
                {
                    cJSON_AddStringToObject(hint, "type",
                        get_agent_content_type(TYPE_TEXT));
                    cJSON_AddStringToObject(hint,
                        get_agent_content_type(TYPE_TEXT),
                        "[语音消息转写失败，请再说一次]");
                    cJSON_AddItemToArray(content_arr, hint);
                }
                break;
#else
                {
                    char out_url[128] = {0};
                    const char *uploaded_url = agent_up_load(
                        messages_get_content_idx(messages, i));
                    /* 上传完成后本地文件不再需要 */
                    unlink(messages_get_content_idx(messages, i));
                    if (!uploaded_url || uploaded_url[0] == '\0')
                    {
                        LOG_E("[agent] audio upload fail, skip item");
                        continue;
                    }
                    rt_strncpy(out_url, uploaded_url, sizeof(out_url) - 1);
                    out_url[sizeof(out_url) - 1] = '\0';

                    cJSON *media = cJSON_CreateObject();
                    cJSON *url_obj = cJSON_CreateObject();
                    if (!media || !url_obj)
                    {
                        if (media) cJSON_Delete(media);
                        if (url_obj) cJSON_Delete(url_obj);
                        continue;
                    }
                    cJSON_AddStringToObject(media, "type",
                        get_agent_content_type(TYPE_AUDIO));
                    cJSON_AddStringToObject(url_obj, "url", out_url);
                    cJSON_AddItemToObject(media,
                        get_agent_content_type(TYPE_AUDIO), url_obj);
                    cJSON_AddItemToArray(content_arr, media);
                }
                break;
#endif /* PKG_AGENT_STT_ENABLE */
            }

            case TYPE_IMAGE:
            case TYPE_VIDEO:
            {
                char out_url[128] = {0};
                const char *uploaded_url = agent_up_load(messages_get_content_idx(messages, i));
                if (!uploaded_url || uploaded_url[0] == '\0')
                {
                    LOG_E("[agent] %s upload fail, skip item",
                          (messages_get_type_idx(messages, i) == TYPE_AUDIO) ? "audio" :
                          (messages_get_type_idx(messages, i) == TYPE_IMAGE) ? "image" : "video");
                    continue;
                }
                rt_strncpy(out_url, uploaded_url, sizeof(out_url) - 1);
                out_url[sizeof(out_url) - 1] = '\0';

                cJSON *media = cJSON_CreateObject();
                cJSON *url_obj = cJSON_CreateObject();
                if (!media || !url_obj)
                {
                    if (media) cJSON_Delete(media);
                    if (url_obj) cJSON_Delete(url_obj);
                    continue;
                }

                const char *type_str = get_agent_content_type(messages_get_type_idx(messages, i));
                cJSON_AddStringToObject(media, "type", type_str);
                cJSON_AddStringToObject(url_obj, "url", out_url);
                cJSON_AddItemToObject(media, type_str, url_obj);
                cJSON_AddItemToArray(content_arr, media);
                break;
            }

            default:
                LOG_E("[agent] unknown message_type:%d, skip", messages_get_type_idx(messages, i));
                break;
        }
    }

    return content_arr;
#endif
}

/*
 * @brief 创建工具项 JSON 对象
 * @param func_name  函数名
 * @param desc       函数描述
 * @param params_obj 参数对象
 * @return 工具项 cJSON 对象
 */
cJSON *create_tool_item(
    const char *func_name,
    const char *desc,
    cJSON *params_obj)
{
    cJSON *tool_root = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_root, "type", "function");

    cJSON *func_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(func_obj, "name", func_name);
    cJSON_AddStringToObject(func_obj, "description", desc);
    cJSON_AddItemToObject(func_obj, "parameters", params_obj);

    cJSON_AddItemToObject(tool_root, "function", func_obj);
    return tool_root;
}

/*
 * @brief 创建参数对象 { type: "object", properties: {...}, required: [...] }
 * @param props_obj    属性对象
 * @param required_arr 必填数组
 * @return 参数 cJSON 对象
 */
cJSON *create_param_obj(
    cJSON *props_obj,
    cJSON *required_arr)
{
    cJSON *param_root = cJSON_CreateObject();
    cJSON_AddStringToObject(param_root, "type", "object");
    cJSON_AddItemToObject(param_root, "properties", props_obj);
    cJSON_AddItemToObject(param_root, "required", required_arr);
    return param_root;
}

/*
 * @brief 创建单个属性并添加到属性对象
 * @param props_obj 属性对象
 * @param name      属性名
 * @param type      属性类型
 * @param desc      属性描述
 */
void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc)
{
    cJSON *prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", type);
    cJSON_AddStringToObject(prop, "description", desc);

    cJSON_AddItemToObject(props_obj, name, prop);
}

/* 文件服务器 - 多模态 */
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

#define IO_BUF_SZ 1024
    io_buf = (unsigned char *)rt_malloc(IO_BUF_SZ);
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
    webclient_set_timeout(session, 120000);

    /* 1. 发送 multipart 头部 */
    wlen = rt_snprintf((char *)io_buf, IO_BUF_SZ,
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
    while ((rlen = read(fd, io_buf, IO_BUF_SZ)) > 0)
    {
        if (webclient_write(session, io_buf, rlen) != rlen)
        {
            rc = -WEBCLIENT_ERROR;
            goto cleanup;
        }
    }

    /* 3. 发送尾部边界 */
    wlen = rt_snprintf((char *)io_buf, IO_BUF_SZ, "\r\n--%s--\r\n", boundary);
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
            int to_read = (resp_data_len < IO_BUF_SZ) ? resp_data_len : IO_BUF_SZ;
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
            int to_read = (resp_data_len < IO_BUF_SZ) ? resp_data_len : IO_BUF_SZ;
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

#define STT_IO_BUF_SZ 1024
    io_buf = (unsigned char *)rt_malloc(STT_IO_BUF_SZ);
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
    webclient_set_timeout(session, 120000);

    /* prefix */
    wlen = rt_snprintf((char *)io_buf, STT_IO_BUF_SZ,
            "--%s\r\nContent-Disposition: form-data; %s\r\n"
            "Content-Type: audio/wav\r\n\r\n",
            boundary, form_data);
    if (webclient_write(session, io_buf, wlen) != wlen) { rc = -1; goto out; }

    /* file body */
    while ((rlen = read(fd, io_buf, STT_IO_BUF_SZ)) > 0)
    {
        if (webclient_write(session, io_buf, rlen) != rlen) { rc = -1; goto out; }
    }

    /* suffix */
    wlen = rt_snprintf((char *)io_buf, STT_IO_BUF_SZ, "\r\n--%s--\r\n", boundary);
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

#endif