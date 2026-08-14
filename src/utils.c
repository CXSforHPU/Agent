#include "utils.h"

void print_reasoning(const char *text)
{

    LOG_I("Reasoning: \n%s", text);
    return;
}

void print_tool_call(const char *text)
{

    LOG_I("Tool Call: \n%s", text);
    return;
}

void print_context(const char *text)
{
    rt_kprintf("%s", text);
    return;
}

/*
@function: format_text
@description: 将发送文本格式化为JSON对象
@input: const char* text - 要格式化的文本
@output:
{
    "text": "要发送的文本",
    "type": "text"
}
*/
cJSON* format_text(const char *text)
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

/* @function: create_content_array
 * @description: 创建一个空的内容数组
 * @return: cJSON* - 创建的内容数组对象
 * []
 */
cJSON* create_content_array()
{

    cJSON *content_array = cJSON_CreateArray();
    if (content_array == NULL)
    {
        return NULL;
    }
    return content_array;
}

/* @function: to_content
 * @description: 将文本添加到内容数组中
 * @param: cJSON* content_array - 内容数组对象
 * @param: const char* text - 要添加的文本
 * @return: cJSON* - 更新后的内容数组对象
 * 多模态
[
        {
            "type": "image_url",
            "image_url": { 
                "url": "data:application/pdf;base64," + base64.b64encode(
                            open("xxx.pdf", "rb").read()).decode("utf-8")
                            }
        },
        {
            "type": "text",
            "text": "<image>\n<|grounding|>Convert the document to markdown. "
        }
]
非多模态
{"content":"xxx"}
 */
cJSON* to_content(Message_t message)
{
    if (!message) {
        return RT_NULL;
    }

#if defined(PKG_AGENT_MULTIMODAL_DISABLE)
    /* 非多模态：返回一个对象，包含 "content" 字段 */
    cJSON* content_obj = cJSON_CreateObject();
    if (!content_obj) {
        return RT_NULL;
    }
    if (message->content) {
        cJSON_AddStringToObject(content_obj, "content", message->content);
    }
    return content_obj;

#elif defined(PKG_AGENT_MULTIMODAL_ENABLE)
    /* 多模态：返回一个数组，每个元素为 { type, ... } */
    cJSON* content_arr = cJSON_CreateArray();
    if (!content_arr) {
        return RT_NULL;
    }

    Message_t p = message;
    for (int i = 0; i < p->size; i++) {
        Message_t item = &p[i];
        if (!item->content) {
            continue;
        }

        switch (item->message_type) {
            case TYPE_TEXT: {
                cJSON* text = cJSON_CreateObject();
                if (!text) continue;
                cJSON_AddStringToObject(text, "type", get_agent_content_type(TYPE_TEXT));
                cJSON_AddStringToObject(text, get_agent_content_type(TYPE_TEXT), item->content);
                cJSON_AddItemToArray(content_arr, text);
                break;
            }

            case TYPE_AUDIO:
            case TYPE_IMAGE:
            case TYPE_VIDEO: {
                char out_url[128] = {0};
                const char* uploaded_url = agent_up_load(item->content);
                if (!uploaded_url || uploaded_url[0] == '\0') {
                    LOG_E("[agent] %s upload fail, skip item",
                          (item->message_type == TYPE_AUDIO) ? "audio" :
                          (item->message_type == TYPE_IMAGE) ? "image" : "video");
                    continue;
                }
                rt_strncpy(out_url, uploaded_url, sizeof(out_url) - 1);
                out_url[sizeof(out_url) - 1] = '\0';

                cJSON* media = cJSON_CreateObject();
                cJSON* url_obj = cJSON_CreateObject();
                if (!media || !url_obj) {
                    if (media) cJSON_Delete(media);
                    if (url_obj) cJSON_Delete(url_obj);
                    continue;
                }

                const char* type_str = get_agent_content_type(item->message_type);
                cJSON_AddStringToObject(media, "type", type_str);
                cJSON_AddStringToObject(url_obj, "url", out_url);
                cJSON_AddItemToObject(media, type_str, url_obj);
                cJSON_AddItemToArray(content_arr, media);
                break;
            }

            default:
                LOG_E("[agent] unknown message_type:%d, skip", item->message_type);
                break;
        }
    }

    return content_arr;
#endif
}



/* @function: create_tool_item
 * @description: 创建tool外层工具
 * @param: const char* func_name ：工具名称
 * @param: const char* desc  :工具描述
 * @return: cJSON* tool_root : 单个工具信息
    {
        'type': 'function',
        'function': {
            'name': 'get_weather',
            'description': 'Get the current weather for a given city.',
            'parameters': {
                'type': 'object',
                'properties': {
                    'city': {
                        'type': 'string',
                        'description': 'The name of the city to query weather for.',
                    },
                },
                'required': ['city'],
            },
        }
    }
 */
cJSON* create_tool_item(
    const char* func_name,
    const char* desc,
    cJSON* params_obj
)
{
    // 外层工具对象 { "type":"function", "function": {...} }
    cJSON* tool_root = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_root, "type", "function");

    // function 子对象
    cJSON* func_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(func_obj, "name", func_name);
    cJSON_AddStringToObject(func_obj, "description", desc);
    cJSON_AddItemToObject(func_obj, "parameters", params_obj);

    cJSON_AddItemToObject(tool_root, "function", func_obj);
    return tool_root;
}

/* @function: create_param_obj
 * @description: 创建parameters
 * @param: cJSON* props_arr: 参数列表
 * @param: cJSON* required_arr: 必要参数列表
 * @return: cJSON* params_root : 参数对象
    'parameters': {
        'type': 'object',
        'properties': {
            'city': {
                'type': 'string',
                'description': 'The name of the city to query weather for.',
            },
        },
        'required': ['city'],
    }
 */
cJSON* create_param_obj(
    cJSON* props_obj,
    cJSON* required_arr
)
{
    cJSON* param_root = cJSON_CreateObject();
    cJSON_AddStringToObject(param_root, "type", "object");
    cJSON_AddItemToObject(param_root, "properties", props_obj);
    cJSON_AddItemToObject(param_root, "required", required_arr);
    return param_root;
}

/* @function: create_property
 * @description: 创建单个参数
 * @param: cJSON* props_obj:参数字典
 * @param: const char* name：参数名称
 * @param: const char* type:参数类型
 * @param: const char* desc:参数描述
 * @return: cJSON* prop : 单个参数信息
{
    "city":{
        'type': 'string',
        'description': 'The name of the city to query weather for.',
    }
}
 */
void create_property(cJSON* props_obj,const char* name,const char* type, const char* desc)
{
    cJSON* prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", type);
    cJSON_AddStringToObject(prop, "description", desc);

    cJSON_AddItemToObject(props_obj,name,prop);
}


/* 文件服务器 多模态启用*/
#ifdef PKG_AGENT_MULTIMODAL_ENABLE
#include "dfs_posix.h"
#include "unistd.h"


/* ==================== 静态全局变量 ==================== */
static rt_mq_t g_file_op_mq_input = RT_NULL;
static rt_mq_t g_file_op_mq_output = RT_NULL;
static rt_thread_t g_file_op_thread = RT_NULL;

/* ==================== 工具函数 ==================== */
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

// /* ==================== 上传函数（内存版，保留但未使用） ==================== */
// static int agent_post_file(const char* URI, const char* filename,
//                            const char* form_data, char *out_resp_buf,
//                            size_t out_resp_len)
// {
//     size_t file_size, total_len;
//     char boundary[60];
//     int fd = -1, rc = WEBCLIENT_OK;
//     unsigned char *body_buf = RT_NULL;
//     struct webclient_session* session = RT_NULL;
//     int bytes_written, bytes_read;

//     if (!URI || !filename || !form_data)
//         return -WEBCLIENT_FILE_ERROR;

//     fd = open(filename, O_RDONLY);
//     if (fd < 0) {
//         LOG_D("open file %s fail", filename);
//         return -WEBCLIENT_FILE_ERROR;
//     }
//     file_size = lseek(fd, 0, SEEK_END);
//     lseek(fd, 0, SEEK_SET);

//     rt_snprintf(boundary, sizeof(boundary), "----------------------------%012d", rt_tick_get());

//     size_t prefix_len = rt_snprintf(RT_NULL, 0,
//             "--%s\r\n"
//             "Content-Disposition: form-data; %s\r\n"
//             "Content-Type: application/octet-stream\r\n\r\n",
//             boundary, form_data);
//     size_t suffix_len = rt_strlen(boundary) + 8;
//     total_len = prefix_len + file_size + suffix_len;

//     body_buf = (unsigned char *)rt_malloc(total_len);
//     if (body_buf == RT_NULL) {
//         rc = -WEBCLIENT_NOMEM;
//         goto cleanup;
//     }

//     unsigned char *ptr = body_buf;
//     ptr += rt_snprintf((char*)ptr, total_len - (ptr - body_buf),
//             "--%s\r\n"
//             "Content-Disposition: form-data; %s\r\n"
//             "Content-Type: application/octet-stream\r\n\r\n",
//             boundary, form_data);
//     ssize_t read_len = read(fd, ptr, file_size);
//     if (read_len != (ssize_t)file_size) {
//         rc = -WEBCLIENT_FILE_ERROR;
//         goto cleanup;
//     }
//     ptr += file_size;
//     ptr += rt_snprintf((char*)ptr, total_len - (ptr - body_buf),
//             "\r\n--%s--\r\n", boundary);

//     session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
//     if (session == RT_NULL) {
//         rc = -WEBCLIENT_NOMEM;
//         goto cleanup;
//     }
//     webclient_set_timeout(session, 30000);

//     webclient_header_fields_add(session, "Content-Length: %zu\r\n", total_len);
//     webclient_header_fields_add(session, "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);

//     rc = webclient_post(session, URI, NULL, 0);
//     if (rc < 0)
//         goto cleanup;

//     bytes_written = webclient_write(session, body_buf, total_len);
//     if (bytes_written != (int)total_len) {
//         rc = -WEBCLIENT_ERROR;
//         goto cleanup;
//     }

//     extern int webclient_handle_response(struct webclient_session *session);
//     if (webclient_handle_response(session) != 200) {
//         rc = -WEBCLIENT_ERROR;
//         goto cleanup;
//     }

//     int resp_len = webclient_content_length_get(session);
//     if (resp_len > 0 && out_resp_buf && out_resp_len > 0) {
//         unsigned char temp[WEBCLIENT_RESPONSE_BUFSZ];
//         int copied = 0;
//         rt_memset(out_resp_buf, 0, out_resp_len);
//         while (resp_len > 0) {
//             int to_read = (resp_len < (int)sizeof(temp)) ? resp_len : (int)sizeof(temp);
//             bytes_read = webclient_read(session, temp, to_read);
//             if (bytes_read <= 0) break;
//             int copy_now = (copied + bytes_read < (int)out_resp_len - 1) ? bytes_read : (int)(out_resp_len - 1 - copied);
//             if (copy_now > 0) {
//                 rt_memcpy(out_resp_buf + copied, temp, copy_now);
//                 copied += copy_now;
//             }
//             resp_len -= bytes_read;
//         }
//     } else {
//         unsigned char temp[WEBCLIENT_RESPONSE_BUFSZ];
//         while (webclient_read(session, temp, sizeof(temp)) > 0) { }
//     }
//     rc = WEBCLIENT_OK;

// cleanup:
//     if (fd >= 0) close(fd);
//     if (session) webclient_close(session);
//     if (body_buf) rt_free(body_buf);
//     return rc;
// }

/* ==================== 流式上传（推荐使用） ==================== */
static int agent_post_file_stream(const char* URI, const char* filename,
                                  const char* form_data, char *out_resp_buf, size_t out_resp_len)
{
    rt_off_t file_sz;
    size_t total_length;
    char boundary[60];
    int fd = -1, rc = WEBCLIENT_OK;
    unsigned char *io_buf = RT_NULL;
    struct webclient_session* session = RT_NULL;
    int wlen, rlen, bytes_read;
    int resp_code, resp_data_len;

    if (!URI || !filename || !form_data)
        return -WEBCLIENT_FILE_ERROR;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        LOG_D("open file %s fail", filename);
        return -WEBCLIENT_FILE_ERROR;
    }
    file_sz = lseek(fd, 0, SEEK_END);
    if (file_sz < 0) {
        rc = -WEBCLIENT_FILE_ERROR;
        goto cleanup;
    }
    lseek(fd, 0, SEEK_SET);

#define IO_BUF_SZ 1024
    io_buf = (unsigned char *)rt_malloc(IO_BUF_SZ);
    if (io_buf == RT_NULL) {
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
    if (session == RT_NULL) {
        rc = -WEBCLIENT_NOMEM;
        goto cleanup;
    }
    webclient_set_timeout(session, 30000);

    webclient_header_fields_add(session, "Content-Length: %zu\r\n", total_length);
    webclient_header_fields_add(session, "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);

    rc = webclient_post(session, URI, NULL, 0);
    if (rc < 0) {
        LOG_D("webclient_post stream fail %d", rc);
        goto cleanup;
    }

    /* 1. 发送 multipart 头部 */
    wlen = rt_snprintf((char*)io_buf, IO_BUF_SZ,
            "--%s\r\n"
            "Content-Disposition: form-data; %s\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            boundary, form_data);
    if (webclient_write(session, io_buf, wlen) != wlen) {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 2. 分片发送文件内容 */
    while ((rlen = read(fd, io_buf, IO_BUF_SZ)) > 0) {
        if (webclient_write(session, io_buf, rlen) != rlen) {
            rc = -WEBCLIENT_ERROR;
            goto cleanup;
        }
    }

    /* 3. 发送尾部边界 */
    wlen = rt_snprintf((char*)io_buf, IO_BUF_SZ, "\r\n--%s--\r\n", boundary);
    if (webclient_write(session, io_buf, wlen) != wlen) {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 处理响应 */
    extern int webclient_handle_response(struct webclient_session *session);
    if (webclient_handle_response(session) != 200) {
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }
    resp_code = webclient_resp_status_get(session);
    if (resp_code < 200 || resp_code >= 300) {
        LOG_D("upload http code:%d", resp_code);
        rc = -WEBCLIENT_ERROR;
        goto cleanup;
    }

    /* 读取响应体 */
    resp_data_len = webclient_content_length_get(session);
    if (resp_data_len > 0 && out_resp_buf && out_resp_len > 0) {
        int copied = 0;
        rt_memset(out_resp_buf, 0, out_resp_len);
        while (resp_data_len > 0) {
            int to_read = (resp_data_len < IO_BUF_SZ) ? resp_data_len : IO_BUF_SZ;
            bytes_read = webclient_read(session, io_buf, to_read);
            if (bytes_read <= 0) break;
            int copy_now = (copied + bytes_read < (int)out_resp_len - 1) ? bytes_read : (int)(out_resp_len - 1 - copied);
            if (copy_now > 0) {
                rt_memcpy(out_resp_buf + copied, io_buf, copy_now);
                copied += copy_now;
            }
            resp_data_len -= bytes_read;
        }
    } else {
        while (resp_data_len > 0) {
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

/* ==================== 下载函数 ==================== */
static int agent_multi_get_file(const char* file_id, const char* path)
{
    char url[128] = {0};
    rt_snprintf(url, sizeof(url), "%s/%s", PKG_AGENT_DOWNLOAD_URL, file_id);
    LOG_I("[download] url: %s -> %s", url, path);
    int ret = webclient_get_file(url, path);
    if (ret == 0) {
        LOG_I("[download] success");
        return RT_EOK;
    } else {
        LOG_E("[download] failed %d", ret);
        return -RT_ERROR;
    }
}

/* ==================== 上传封装（解析JSON） ==================== */
static rt_err_t agent_upload_file(const char* path, char *out_url, size_t out_url_len)
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
    if (upload_ret != WEBCLIENT_OK) {
        LOG_E("upload file failed, err=%d", upload_ret);
        return -RT_ERROR;
    }

    LOG_I("upload success, rsp: %s", resp_buf);

    resp_json = cJSON_Parse(resp_buf);
    if (resp_json == RT_NULL) {
        LOG_E("json parse fail");
        goto exit_fail;
    }

    cJSON *data = cJSON_GetObjectItem(resp_json, "data");
    if (!data) {
        LOG_E("no data field");
        goto exit_fail;
    }
    cJSON *file_id_json = cJSON_GetObjectItem(data, "file_id");
    if (!file_id_json || !cJSON_IsString(file_id_json) || !file_id_json->valuestring) {
        LOG_E("no file_id field");
        goto exit_fail;
    }

    int sn = rt_snprintf(out_url, out_url_len, "%s/%s",
                         PKG_AGENT_DOWNLOAD_URL, file_id_json->valuestring);
    if (sn < 0 || sn >= (int)out_url_len) {
        LOG_E("url buffer overflow");
        goto exit_fail;
    }

    ret = RT_EOK;
exit_fail:
    if (resp_json) cJSON_Delete(resp_json);
    return ret;
}

/* ==================== 工作线程 ==================== */
static void file_operation_work_thread(void *arg)
{
    file_op_req req, req_out;
    char url[128] = {0};
    rt_err_t ret;

    while (1) {
        rt_memset(&req, 0, sizeof(req));
        rt_mq_recv(g_file_op_mq_input, &req, sizeof(req), RT_WAITING_FOREVER);

        if (req.op == CMD_AGENT_FILE_EXIT) {
            LOG_I("[fileop] exit thread");
            break;
        }

        switch (req.op) {
            case CMD_AGENT_FILE_UPLOAD: {
                rt_kprintf("[upload] start %s\n", req.path);
                ret = agent_upload_file(req.path, url, sizeof(url));
                rt_kprintf("[upload] %s, url=%s\n", (ret == RT_EOK) ? "ok" : "failed", url);

                /* 构造上传结果消息 */
                rt_memset(&req_out, 0, sizeof(req_out));
                req_out.op = CMD_AGENT_FILE_URL;
                /* 从 url 中提取 file_id（最后一个'/'之后的部分） */
                const char *p = strrchr(url, '/');
                if (p) p++;
                else p = url;
                rt_strncpy(req_out.file_id, p, sizeof(req_out.file_id) - 1);
                req_out.file_id[sizeof(req_out.file_id) - 1] = '\0';
                /* 复制完整 URL 到 path 字段 */
                rt_strncpy(req_out.path, url, sizeof(req_out.path) - 1);
                req_out.path[sizeof(req_out.path) - 1] = '\0';

                rt_mq_send(g_file_op_mq_output, &req_out, sizeof(req_out));
                break;
            }

            case CMD_AGENT_FILE_DOWNLOAD: {
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

/* ==================== 对外接口 ==================== */
rt_mq_t get_agent_file_op_mq_input(void)
{
    return g_file_op_mq_input;
}

rt_mq_t get_agent_file_op_mq_output(void)
{
    return g_file_op_mq_output;    /* 修复：原来返回了 input */
}

/* 初始化 */
rt_thread_t agent_file_op_init(void)
{
    g_file_op_mq_input = rt_mq_create("fileop_mq_in", sizeof(file_op_req), 4, RT_IPC_FLAG_FIFO);
    if (g_file_op_mq_input == RT_NULL)
        return RT_NULL;

    g_file_op_mq_output = rt_mq_create("fileop_mq_out", sizeof(file_op_req), 4, RT_IPC_FLAG_FIFO);
    if (g_file_op_mq_output == RT_NULL) {
        rt_mq_delete(g_file_op_mq_input);
        g_file_op_mq_input = RT_NULL;
        return RT_NULL;
    }

    g_file_op_thread = rt_thread_create("fileop_wk",
                                        file_operation_work_thread,
                                        RT_NULL,
                                        PKG_AGENT_FILE_OP_THREAD_SIZE,
                                        9,
                                        20);
    if (g_file_op_thread == RT_NULL) {
        rt_mq_delete(g_file_op_mq_input);
        rt_mq_delete(g_file_op_mq_output);
        g_file_op_mq_input = RT_NULL;
        g_file_op_mq_output = RT_NULL;
        return RT_NULL;
    }

    rt_thread_startup(g_file_op_thread);
    return g_file_op_thread;
}

/* 销毁 */
void agent_file_op_deinit(void)
{
    if (g_file_op_mq_input != RT_NULL) {
        file_op_req exit_req;
        exit_req.op = CMD_AGENT_FILE_EXIT;
        rt_mq_send(g_file_op_mq_input, &exit_req, sizeof(file_op_req));
        rt_thread_mdelay(100);
    }

    if (g_file_op_thread != RT_NULL) {
        rt_thread_delete(g_file_op_thread);
        g_file_op_thread = RT_NULL;
    }

    if (g_file_op_mq_input != RT_NULL) {
        rt_mq_delete(g_file_op_mq_input);
        g_file_op_mq_input = RT_NULL;
    }
    if (g_file_op_mq_output != RT_NULL) {
        rt_mq_delete(g_file_op_mq_output);
        g_file_op_mq_output = RT_NULL;
    }
    LOG_I("[fileop] deinit done");
}

/* 下载任务入队 */
rt_err_t down_load(const char* path, const char* file_id)
{
    if (g_file_op_mq_input == RT_NULL) {
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

    if (rt_mq_send(g_file_op_mq_input, &req, sizeof(req)) != RT_EOK) {
        LOG_E("send download request failed");
        return -RT_ERROR;
    }

    LOG_I("download task enqueued, file_id=%s", file_id);
    return RT_EOK;
}

/* 上传任务（同步等待结果，返回 URL 静态指针） */
char* agent_up_load(const char* path)
{
    /* 使用静态缓冲区保存 URL，调用者需及时拷贝 */
    static char result_url[128];
    rt_memset(result_url,0,sizeof(result_url));

    if (g_file_op_mq_input == RT_NULL || g_file_op_mq_output == RT_NULL) {
        LOG_E("file op mq not init");
        return RT_NULL;
    }

    file_op_req req_input, req_output;
    rt_memset(&req_input, 0, sizeof(req_input));
    rt_memset(&req_output, 0, sizeof(req_output));

    req_input.op = CMD_AGENT_FILE_UPLOAD;
    rt_strncpy(req_input.path, path, sizeof(req_input.path) - 1);
    req_input.path[sizeof(req_input.path) - 1] = '\0';

    if (rt_mq_send(g_file_op_mq_input, &req_input, sizeof(req_input)) != RT_EOK) {
        LOG_E("send upload request failed");
        return RT_NULL;
    }

    rt_mq_recv(g_file_op_mq_output, &req_output, sizeof(req_output), RT_WAITING_FOREVER);

    rt_strncpy(result_url, req_output.path, sizeof(result_url) - 1);
    result_url[sizeof(result_url) - 1] = '\0';

    LOG_I("upload complete, url=%s", result_url);
    return result_url;
}

#endif