/*
 * 自发上报通道：agent 自己产生的结论（MQTT 告警、传感器越限等）推给控制台。
 *
 * 控制台通过 A2A 的 POST /tasks/pushNotificationConfig 下发回调地址，
 * 板端存进 SD 卡；此后每条自发结论都 POST 到那个地址。
 * 上报走独立线程，不占 webnet 的连接线程，也不占 agent 主循环。
 */
#include "utils/agent_report.h"
#include "utils/webclient_util.h"

#include <webclient.h>
#include <cJSON.h>
#include <dfs_posix.h>
#include <unistd.h>

#define LOG_TAG "Agent.report"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/* 上报请求的收发超时 */
#define AGENT_REPORT_TIMEOUT_MS   5000
/* 上报线程栈 */
#define AGENT_REPORT_THREAD_STACK 4096
#define AGENT_REPORT_THREAD_PRIO  12

static char s_url[AGENT_REPORT_URL_MAX];
static char s_id[AGENT_REPORT_ID_MAX];
static char s_queue[AGENT_REPORT_QUEUE_SIZE][AGENT_REPORT_TEXT_MAX];
static rt_uint16_t s_head;                 /* 队首 */
static rt_uint16_t s_tail;                 /* 队尾 */
static rt_uint16_t s_count;                /* 队列里已有多少条 */

static struct rt_mutex s_lock;
static rt_sem_t s_sem = RT_NULL;
static rt_thread_t s_thread = RT_NULL;
static volatile rt_bool_t s_stop = RT_FALSE;
static rt_bool_t s_inited = RT_FALSE;

/* ==========================================================================
 * 配置文件读写
 * ========================================================================== */

/*
 * @brief 把当前回调配置写回 SD 卡
 * @return RT_EOK 成功
 */
static rt_err_t report_save(void)
{
    cJSON *root;
    char *encoded;
    rt_size_t len;
    int fd;
    int written;

    root = cJSON_CreateObject();
    if (root == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(root, "url", s_url);
    cJSON_AddStringToObject(root, "id", s_id);

    encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    len = rt_strlen(encoded);

    fd = open(PKG_AGENT_A2A_PUSH_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        LOG_W("report: cannot write %s", PKG_AGENT_A2A_PUSH_PATH);
        cJSON_free(encoded);
        return -RT_ERROR;
    }
    written = write(fd, encoded, len);
    close(fd);
    cJSON_free(encoded);

    return (written == (int)len) ? RT_EOK : -RT_ERROR;
}

/*
 * @brief 读回上次下发的回调配置
 * @note  文件不存在是正常情况（第一次启动），不当错误报
 */
static void report_load(void)
{
    char buf[AGENT_REPORT_URL_MAX + AGENT_REPORT_ID_MAX + 32];
    cJSON *root;
    cJSON *item;
    int fd;
    int len;

    fd = open(PKG_AGENT_A2A_PUSH_PATH, O_RDONLY);
    if (fd < 0)
    {
        return;
    }
    len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
    {
        return;
    }
    buf[len] = '\0';

    root = cJSON_Parse(buf);
    if (root == RT_NULL)
    {
        LOG_W("report: %s is not valid JSON, ignoring", PKG_AGENT_A2A_PUSH_PATH);
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (cJSON_IsString(item) && item->valuestring != RT_NULL && item->valuestring[0] != '\0')
    {
        rt_snprintf(s_url, sizeof(s_url), "%s", item->valuestring);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(item) && item->valuestring != RT_NULL)
    {
        rt_snprintf(s_id, sizeof(s_id), "%s", item->valuestring);
    }

    cJSON_Delete(root);
    if (s_url[0] != '\0')
    {
        LOG_I("report: callback loaded: %s", s_url);
    }
}

/* ==========================================================================
 * 队列
 * ========================================================================== */

/*
 * @brief 入队一条自发结论
 */
rt_err_t agent_report_push(const char *text)
{
    rt_err_t rc = RT_EOK;

    if (text == RT_NULL || text[0] == '\0')
    {
        return RT_ERROR;
    }
    if (!s_inited)
    {
        return RT_ERROR;
    }

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_count >= AGENT_REPORT_QUEUE_SIZE)
    {
        /* 队列满了丢最旧的一条，位置不动 */
        s_head = (rt_uint16_t)((s_head + 1) % AGENT_REPORT_QUEUE_SIZE);
        s_count--;
        LOG_W("report: queue full, drop the oldest event");
    }
    rt_snprintf(s_queue[s_tail], AGENT_REPORT_TEXT_MAX, "%s", text);
    s_tail = (rt_uint16_t)((s_tail + 1) % AGENT_REPORT_QUEUE_SIZE);
    s_count++;
    rt_mutex_release(&s_lock);

    if (s_sem != RT_NULL)
    {
        rt_sem_release(s_sem);
    }
    return rc;
}

/*
 * @brief 取出一条待上报的结论文本
 */
static rt_err_t report_pop(char *out, rt_size_t size)
{
    rt_err_t rc = RT_ERROR;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_count > 0)
    {
        rt_snprintf(out, size, "%s", s_queue[s_head]);
        s_head = (rt_uint16_t)((s_head + 1) % AGENT_REPORT_QUEUE_SIZE);
        s_count--;
        rc = RT_EOK;
    }
    rt_mutex_release(&s_lock);

    return rc;
}

/* ==========================================================================
 * 上报
 * ========================================================================== */

/*
 * @brief 把一条结论 POST 给控制台
 * @param text 结论文本
 * @return RT_EOK 成功
 */
static rt_err_t report_post(const char *text)
{
    struct webclient_session *session;
    cJSON *root;
    char *body;
    rt_size_t body_len;
    int status;

    if (s_url[0] == '\0')
    {
        return -RT_ERROR;
    }

    root = cJSON_CreateObject();
    if (root == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(root, "kind", "report");
    cJSON_AddStringToObject(root, "id", s_id);
    cJSON_AddStringToObject(root, "text", text);

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    body_len = rt_strlen(body);

    /* 响应头比请求头长（Tomcat 会带 Vary/Date 等头），给足 512 字节 */
    session = webclient_session_create(512);
    if (session == RT_NULL)
    {
        cJSON_free(body);
        return -RT_ENOMEM;
    }

    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", (int)body_len);

    /* 先连上再发请求体：webclient 的超时只能设在已经连上的 socket 上 */
    status = webclient_post(session, s_url, RT_NULL, 0);
    if (status < 0)
    {
        LOG_W("report: connect failed: %d errno %d", status, errno);
        webclient_close(session);
        cJSON_free(body);
        return -RT_ERROR;
    }
    agent_webclient_set_timeout(session, AGENT_REPORT_TIMEOUT_MS);

    if (webclient_write(session, body, body_len) != (int)body_len)
    {
        LOG_W("report: send body failed, errno %d", errno);
        webclient_close(session);
        cJSON_free(body);
        return -RT_ERROR;
    }

    status = webclient_handle_response(session);
    if (status != 200)
    {
        /* 日志行有长度上限（ulog 一行 128 字节），这里只打状态码和错误码 */
        LOG_W("report: http %d errno %d", status, errno);
        LOG_W("report: resp %.32s", session->header->buffer);
        webclient_close(session);
        cJSON_free(body);
        return -RT_ERROR;
    }

    LOG_I("report: sent, %d bytes", (int)body_len);
    webclient_close(session);
    cJSON_free(body);
    return RT_EOK;
}

/*
 * @brief 上报线程：等事件，出队，POST 出去
 */
static void report_thread_entry(void *param)
{
    char text[AGENT_REPORT_TEXT_MAX];

    RT_UNUSED(param);

    while (!s_stop)
    {
        if (s_sem != RT_NULL)
        {
            rt_sem_take(s_sem, RT_WAITING_FOREVER);
        }
        if (s_stop)
        {
            break;
        }
        while (report_pop(text, sizeof(text)) == RT_EOK)
        {
            if (report_post(text) != RT_EOK)
            {
                /*
                 * 控制台的应答偶尔会丢（板端 TCP 资源紧张时收不到），
                 * 隔一会儿用新连接重试一次，还不行就放弃这条。
                 */
                rt_thread_mdelay(500);
                if (report_post(text) != RT_EOK)
                {
                    LOG_W("report: give up after 2 tries");
                    break;
                }
            }
        }
    }

    LOG_I("report: thread exit");
}

/*
 * @brief 启动上报线程（只在配了回调地址后才有必要）
 * @return RT_EOK 成功
 */
static rt_err_t report_start(void)
{
    if (s_thread != RT_NULL)
    {
        return RT_EOK;
    }
    if (s_sem == RT_NULL)
    {
        s_sem = rt_sem_create("report", 0, RT_IPC_FLAG_FIFO);
        if (s_sem == RT_NULL)
        {
            return -RT_ENOMEM;
        }
    }
    s_stop = RT_FALSE;
    s_thread = rt_thread_create("report", report_thread_entry, RT_NULL,
                                AGENT_REPORT_THREAD_STACK, AGENT_REPORT_THREAD_PRIO, 10);
    if (s_thread == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    rt_thread_startup(s_thread);
    return RT_EOK;
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

void agent_report_init(void)
{
    if (s_inited)
    {
        return;
    }
    if (rt_mutex_init(&s_lock, "agentReport", RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return;
    }
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_url[0] = '\0';
    s_id[0] = '\0';
    s_inited = RT_TRUE;

    report_load();
    if (s_url[0] != '\0')
    {
        report_start();
    }
}

void agent_report_deinit(void)
{
    if (!s_inited)
    {
        return;
    }
    s_stop = RT_TRUE;
    if (s_sem != RT_NULL)
    {
        rt_sem_release(s_sem);
    }
    if (s_thread != RT_NULL)
    {
        rt_thread_delete(s_thread);
        s_thread = RT_NULL;
    }
    if (s_sem != RT_NULL)
    {
        rt_sem_delete(s_sem);
        s_sem = RT_NULL;
    }
    rt_mutex_detach(&s_lock);
    s_inited = RT_FALSE;
}

rt_err_t agent_report_set_callback(const char *url, const char *id)
{
    char new_url[AGENT_REPORT_URL_MAX];
    char new_id[AGENT_REPORT_ID_MAX];

    if (!s_inited)
    {
        return -RT_ERROR;
    }

    new_url[0] = '\0';
    new_id[0] = '\0';
    if (url != RT_NULL && url[0] != '\0')
    {
        if (rt_strlen(url) >= sizeof(new_url))
        {
            LOG_W("report: callback url too long");
            return -RT_EINVAL;
        }
        rt_strncpy(new_url, url, sizeof(new_url) - 1);
        new_url[sizeof(new_url) - 1] = '\0';
    }
    if (id != RT_NULL)
    {
        rt_strncpy(new_id, id, sizeof(new_id) - 1);
        new_id[sizeof(new_id) - 1] = '\0';
    }

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (rt_strcmp(s_url, new_url) == 0 && rt_strcmp(s_id, new_id) == 0)
    {
        rt_mutex_release(&s_lock);
        return RT_EOK;                 /* 地址没变，不用再写 SD 卡 */
    }
    rt_strncpy(s_url, new_url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    rt_strncpy(s_id, new_id, sizeof(s_id) - 1);
    s_id[sizeof(s_id) - 1] = '\0';
    rt_mutex_release(&s_lock);

    if (s_url[0] == '\0')
    {
        LOG_I("report: callback cleared");
        return RT_EOK;
    }

    if (report_save() != RT_EOK)
    {
        LOG_W("report: failed to persist the callback");
    }
    report_start();
    LOG_I("report: callback set: %s", s_url);
    return RT_EOK;
}

const char *agent_report_callback(void)
{
    return s_url;
}

const char *agent_report_callback_id(void)
{
    return s_id;
}

rt_bool_t agent_report_ready(void)
{
    return (s_url[0] != '\0') ? RT_TRUE : RT_FALSE;
}
