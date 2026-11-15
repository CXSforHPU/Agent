#include "AgentA2aChannel.h"
#include "AgentWebSocket.h"
#include "utils/agent_report.h"
#include <cJSON.h>
#include "AgentStream.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dfs_posix.h>

/*
 * A2A WebSocket 通道：板端主动连 Web。生命周期三态：
 *   未激活  —— 没有 token，不自动连（不每 3 秒乱连），等 a2a_activate；
 *   激活中  —— a2a_activate 用账号密码握手，服务端签发一次性设备 token；
 *   已激活  —— token 落 SD 卡，之后凭 token 自动连接，密码不再出现。
 * 同一设备只能激活一次：服务端对重复激活回 already_activated 错误帧，板端原样打印原因。
 */

#define A2A_FRAME_MAX 8192

#ifndef PKG_AGENT_A2A_CRED_FILE
#define PKG_AGENT_A2A_CRED_FILE "/agent_a2a_cred"
#endif
#ifndef PKG_AGENT_A2A_ENDPOINT_FILE
#define PKG_AGENT_A2A_ENDPOINT_FILE "/agent_a2a_endpoint"
#endif

static MessageHub_t a2a_hub;
static volatile rt_bool_t a2a_running;
static rt_thread_t a2a_thread;
static int a2a_fd = -1;
static rt_sem_t a2a_done;
/* a2a_activate 激活完成后踢醒等工作线程，让它用新 token 去连 */
static rt_sem_t a2a_kick;
/* Web 下发的绑定能力卡（RAM 缓存，每次 welcome 后服务端会重推） */
static cJSON *bound_card;

static int send_frame(int fd, cJSON *frame)
{
    char *bytes = cJSON_PrintUnformatted(frame);
    int result = -1;
    if (bytes) { result = agent_ws_send(fd, 1, bytes, strlen(bytes)); cJSON_free(bytes); }
    return result;
}

static cJSON *read_frame(int fd)
{
    char *bytes = rt_malloc(A2A_FRAME_MAX + 1);
    cJSON *frame = RT_NULL;
    if (!bytes) return RT_NULL;
    {
        int n = agent_ws_read(fd, bytes, A2A_FRAME_MAX + 1);
        if (n > 0) frame = cJSON_Parse(bytes);
        else if (n == -2) frame = cJSON_Parse("{\"type\":\"control\"}");
    }
    rt_free(bytes);
    if (!cJSON_IsObject(frame)) { cJSON_Delete(frame); return RT_NULL; }
    return frame;
}

static int reports(int fd)
{
    char text[AGENT_REPORT_TEXT_MAX];
    while (agent_report_take(text, sizeof(text)) == RT_EOK)
    {
        cJSON *event = cJSON_CreateObject();
        int rc;
        cJSON_AddStringToObject(event, "type", "report");
        cJSON_AddStringToObject(event, "kind", "report");
        cJSON_AddStringToObject(event, "text", text);
        rc = send_frame(fd, event); cJSON_Delete(event);
        if (rc) return -1;
    }
    return 0;
}

static const char *field(cJSON *frame, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(frame, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

/* 能力卡：基础字段取 Kconfig；Web 绑定并推送过能力卡时优先用推送内容。 */
static cJSON *card(void)
{
    cJSON *value = cJSON_CreateObject(), *caps;
    const char *name = PKG_AGENT_A2A_NAME;
    const char *description = PKG_AGENT_A2A_DESCRIPTION;
    const char *version = "1.0.0";
    cJSON *skills = cJSON_CreateArray();
    cJSON *pushed;

    if (bound_card)
    {
        pushed = cJSON_GetObjectItemCaseSensitive(bound_card, "name");
        if (cJSON_IsString(pushed)) name = pushed->valuestring;
        pushed = cJSON_GetObjectItemCaseSensitive(bound_card, "description");
        if (cJSON_IsString(pushed)) description = pushed->valuestring;
        pushed = cJSON_GetObjectItemCaseSensitive(bound_card, "version");
        if (cJSON_IsString(pushed)) version = pushed->valuestring;
        pushed = cJSON_GetObjectItemCaseSensitive(bound_card, "skills");
        if (cJSON_IsArray(pushed))
        {
            cJSON_Delete(skills);
            skills = cJSON_Duplicate(pushed, 1);
        }
    }

    cJSON_AddStringToObject(value, "name", name);
    cJSON_AddStringToObject(value, "description", description);
    cJSON_AddStringToObject(value, "version", version);
    cJSON_AddStringToObject(value, "url", "ws://" PKG_AGENT_A2A_DEVICE_ID);
    caps = cJSON_AddObjectToObject(value, "capabilities");
    cJSON_AddBoolToObject(caps, "streaming", 0);
    cJSON_AddBoolToObject(caps, "pushNotifications", 1);
    cJSON_AddItemToObject(value, "skills", skills);
    return value;
}

/* Exactly one submitted mailbox task may be outstanding, across reconnects.
 * On deadline, quarantine it until its late output is consumed. Never relabel
 * an old output as the result of a new request. */
static rt_bool_t outstanding;
static int handle(int fd, cJSON *request)
{
    const char *type = field(request, "type"), *id = field(request, "id");
    cJSON *reply;
    if (!strcmp(type, "control")) return 0;
    reply = cJSON_CreateObject();
    int result;
    if (!*id || strlen(id) > 64) { cJSON_Delete(reply); return -1; }
    cJSON_AddStringToObject(reply, "id", id);
    if (!strcmp(type, "ping"))
        cJSON_AddStringToObject(reply, "type", "pingResult");
    else if (!strcmp(type, "card"))
    {
        /* 服务端既会查询能力卡，也会把绑定的能力卡推下来（带 card 字段）。 */
        cJSON *pushed = cJSON_GetObjectItemCaseSensitive(request, "card");
        if (cJSON_IsObject(pushed))
        {
            if (bound_card) cJSON_Delete(bound_card);
            bound_card = cJSON_Duplicate(pushed, 1);
        }
        cJSON_AddStringToObject(reply, "type", "cardResult");
        cJSON_AddItemToObject(reply, "card", card());
    }
    else if (!strcmp(type, "task"))
    {
        const char *text = field(request, "text");
        Messages_t input = RT_NULL, output = RT_NULL;
        rt_tick_t start = rt_tick_get();
        cJSON_AddStringToObject(reply, "type", "taskResult");
        if (outstanding)
        {
            output = message_hub_get_timeout(a2a_hub, a2a_hub->output_mailbox, 0);
            if (output) { messages_destroy(output); outstanding = RT_FALSE; }
        }
        if (outstanding || !*text) cJSON_AddStringToObject(reply, "error", "busy or empty task");
        else
        {
            input = messages_create(1);
            if (!input || messages_append(input, TYPE_TEXT, text) != RT_EOK ||
                a2a_hub->put_message(a2a_hub, input, a2a_hub->input_mailbox) != RT_EOK)
            {
                if (input) messages_destroy(input);
                cJSON_AddStringToObject(reply, "error", "task submit failed");
            }
            else
            {
                outstanding = RT_TRUE;
                do {
                    fd_set fds;
                    struct timeval wait = {0, 0};
                    int ready;
                    agent_stream_clear();
                    FD_ZERO(&fds); FD_SET(fd, &fds);
                    ready = select(fd + 1, &fds, RT_NULL, RT_NULL, &wait);
                    if (ready > 0) {
                        char unexpected[256];
                        if (agent_ws_read(fd, unexpected, sizeof(unexpected)) != -2) break;
                    }
                    if (ready < 0 || reports(fd)) break;
                    output = message_hub_get_timeout(a2a_hub, a2a_hub->output_mailbox,
                                                     rt_tick_from_millisecond(100));
                } while (a2a_running && !output && rt_tick_get() - start <
                         rt_tick_from_millisecond(PKG_AGENT_A2A_TASK_TIMEOUT_MS));
                if (output)
                {
                    const char *text_out = messages_get_content_idx(output, 0);
                    outstanding = RT_FALSE;
                    cJSON_AddStringToObject(reply, "state", "completed");
                    cJSON_AddStringToObject(reply, "text", text_out ? text_out : "");
                    messages_destroy(output);
                }
                else cJSON_AddStringToObject(reply, "error", "task timed out; outcome unknown");
            }
        }
    }
    else cJSON_AddStringToObject(reply, "error", "unsupported request");
    result = send_frame(fd, reply);
    cJSON_Delete(reply);
    return result;
}

/* 读一行文件内容（去掉换行）。成功返回 0。 */
static int read_line(const char *path, char *buffer, int capacity)
{
    int fd = open(path, O_RDONLY);
    int n;
    if (fd < 0) return -1;
    n = read(fd, buffer, capacity - 1);
    close(fd);
    if (n <= 0) return -1;
    buffer[n] = 0;
    buffer[strcspn(buffer, "\r\n")] = 0;
    return 0;
}

/* 读设备 token。返回长度，0 表示未激活。 */
static int read_token(char *token, int capacity)
{
    if (read_line(PKG_AGENT_A2A_TOKEN_FILE, token, capacity)) return 0;
    return (int)strlen(token);
}

/* 端点：命令参数 > 激活时保存的端点文件 > Kconfig。 */
static void resolve_endpoint(char *host, int host_capacity, int *port,
                             const char *host_arg, int port_arg)
{
    char line[96];
    if (host_arg && *host_arg)
    {
        rt_strncpy(host, host_arg, host_capacity - 1);
        *port = port_arg > 0 ? port_arg : PKG_AGENT_A2A_PORT;
        return;
    }
    if (read_line(PKG_AGENT_A2A_ENDPOINT_FILE, line, sizeof(line)) == 0)
    {
        char *space = strchr(line, ' ');
        if (space)
        {
            *space = 0;
            rt_strncpy(host, line, host_capacity - 1);
            *port = atoi(space + 1);
            if (*port <= 0) *port = PKG_AGENT_A2A_PORT;
            return;
        }
    }
    rt_strncpy(host, PKG_AGENT_A2A_HOST, host_capacity - 1);
    *port = PKG_AGENT_A2A_PORT;
}

/*
 * 激活凭据：命令参数 > 凭据文件（首行 "账号 密码"）> Kconfig。
 * 运行时文件优先，同一份固件换张 SD 卡就能换成另一台设备的凭据。
 * 密码绝不打印，也绝不写进日志。
 */
static void resolve_credentials(char *username, int user_capacity,
                                char *password, int pass_capacity,
                                const char *user_arg, const char *pass_arg)
{
    char line[192];
    char *space;

    username[0] = 0;
    password[0] = 0;
    if (*PKG_AGENT_A2A_PASSWORD)
        rt_strncpy(password, PKG_AGENT_A2A_PASSWORD, pass_capacity - 1);
    if (*PKG_AGENT_A2A_USERNAME)
        rt_strncpy(username, PKG_AGENT_A2A_USERNAME, user_capacity - 1);
    if (read_line(PKG_AGENT_A2A_CRED_FILE, line, sizeof(line)) == 0)
    {
        space = strchr(line, ' ');
        if (space)
        {
            *space = 0;
            rt_strncpy(username, line, user_capacity - 1);
            rt_strncpy(password, space + 1, pass_capacity - 1);
        }
    }
    if (user_arg && *user_arg) rt_strncpy(username, user_arg, user_capacity - 1);
    if (pass_arg && *pass_arg) rt_strncpy(password, pass_arg, pass_capacity - 1);
}

/* TCP 连接 + WS 升级，失败返回 -1。 */
static int open_socket(const char *host, int port)
{
    struct addrinfo hints, *addresses = RT_NULL;
    char port_text[8];
    int fd;
    struct timeval timeout = {5, 0};

    rt_memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rt_snprintf(port_text, sizeof(port_text), "%d", port);
    if (getaddrinfo(host, port_text, &hints, &addresses)) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(addresses); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, addresses->ai_addr, addresses->ai_addrlen))
    {
        freeaddrinfo(addresses);
        close(fd);
        return -1;
    }
    freeaddrinfo(addresses);
    if (agent_ws_upgrade(fd, host, PKG_AGENT_A2A_WS_ENDPOINT))
    {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * 先写同目录临时文件，再 rename 顶替目标。写失败或掉电时原文件不变，
 * rename 成功后读回比对，确认落盘内容完整。
 */
static int write_file_atomic(const char *path, const char *text)
{
    char temp[96];
    char readback[96];
    int fd, n, length = (int)strlen(text);

    if (length >= (int)sizeof(readback)) return -1;
    rt_snprintf(temp, sizeof(temp), "%s.tmp", path);
    unlink(temp);
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    n = write(fd, text, length);
    close(fd);
    if (n != length)
    {
        unlink(temp);
        return -1;
    }
    if (dfs_file_rename(temp, path))
    {
        unlink(temp);
        return -1;
    }
    if (read_line(path, readback, sizeof(readback)) || strcmp(readback, text))
        return -1;
    return 0;
}

/* 保存激活签发的 token 与端点，重启后凭 token 自动连接。 */
static int save_activation(const char *token, const char *host, int port)
{
    char endpoint[96];
    if (write_file_atomic(PKG_AGENT_A2A_TOKEN_FILE, token)) return -1;
    rt_snprintf(endpoint, sizeof(endpoint), "%s %d", host, port);
    return write_file_atomic(PKG_AGENT_A2A_ENDPOINT_FILE, endpoint);
}

/*
 * 执行一次激活：账号密码 hello → 服务端签发 token → 落 SD 卡。
 * 返回 0 成功；失败返回 -1，原因已打印（不含密码）。
 */
static int do_activate(const char *host, int port, const char *username, const char *password)
{
    char token[257];
    cJSON *hello, *response;
    int fd;
    const char *type;

    if (agent_ws_random_init())
    {
        rt_kprintf("a2a_activate: [FAIL:seed] 缺少随机种子 %s（先写入 48 字节随机数）\n",
                   "/agent_a2a_seed");
        rt_kprintf("a2a_activate: 主机跑 python -c \"import secrets; print(secrets.token_hex(24))\"，\n");
        rt_kprintf("a2a_activate: 再把 48 个十六进制字符写进 /agent_a2a_seed，步骤见包 README 的首次激活一节\n");
        return -1;
    }
    fd = open_socket(host, port);
    if (fd < 0)
    {
        rt_kprintf("a2a_activate: [FAIL:connect] 连不上 %s:%d\n", host, port);
        return -1;
    }
    a2a_fd = fd;
    hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "deviceId", PKG_AGENT_A2A_DEVICE_ID);
    cJSON_AddStringToObject(hello, "username", username);
    cJSON_AddStringToObject(hello, "password", password);
    if (send_frame(fd, hello)) { cJSON_Delete(hello); goto failed; }
    cJSON_Delete(hello);

    response = read_frame(fd);
    if (!response)
    {
        rt_kprintf("a2a_activate: [FAIL:no-response] 服务端无响应\n");
        goto failed;
    }
    type = field(response, "type");
    if (!strcmp(type, "welcome"))
    {
        const char *issued = field(response, "token");
        if (strlen(issued) < 16)
        {
            cJSON_Delete(response);
            rt_kprintf("a2a_activate: [FAIL:no-token] 服务端未下发设备 token\n");
            goto failed;
        }
        rt_strncpy(token, issued, sizeof(token) - 1);
        token[sizeof(token) - 1] = 0;
        cJSON_Delete(response);
        if (save_activation(token, host, port))
        {
            rt_memset(token, 0, sizeof(token));
            rt_kprintf("a2a_activate: [FAIL:save] token 写不进 %s，检查 SD 卡是否只读或已满\n",
                       PKG_AGENT_A2A_TOKEN_FILE);
            rt_kprintf("a2a_activate: 服务端已经把这台设备标成已激活，本地没落盘就连不上了\n");
            rt_kprintf("a2a_activate: 恢复办法：在控制台删掉设备 %s 重新登记，再执行一次 a2a_activate\n",
                       PKG_AGENT_A2A_DEVICE_ID);
            goto failed;
        }
        rt_memset(token, 0, sizeof(token));
        rt_kprintf("a2a_activate: [OK] 激活成功，设备 %s，token 已保存到 %s\n",
                   PKG_AGENT_A2A_DEVICE_ID, PKG_AGENT_A2A_TOKEN_FILE);
        close(fd); a2a_fd = -1;
        if (a2a_kick) rt_sem_release(a2a_kick);
        return 0;
    }
    if (!strcmp(type, "error"))
    {
        const char *code = field(response, "code");
        const char *message = field(response, "message");
        rt_kprintf("a2a_activate: [FAIL:%s] 激活失败，%s\n", code, message);
        cJSON_Delete(response);
        goto failed;
    }
    rt_kprintf("a2a_activate: [FAIL:unexpected] 意外的应答类型 %s\n", type);
    cJSON_Delete(response);

failed:
    close(fd); a2a_fd = -1;
    return -1;
}

/* MSH：a2a_activate [host [port [username [password]]]]
 * 密码只从参数/配置/文件取，任何输出都不回显密码。 */
static int a2a_activate(int argc, char **argv)
{
    char host[64], username[65], password[129], token[257];
    int port = PKG_AGENT_A2A_PORT;
    const char *host_arg = argc > 1 ? argv[1] : RT_NULL;
    const char *user_arg = argc > 3 ? argv[3] : RT_NULL;
    const char *pass_arg = argc > 4 ? argv[4] : RT_NULL;

    if (argc > 2)
    {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535)
        {
            rt_kprintf("usage: a2a_activate [host [port [username [password]]]]\n");
            return -1;
        }
    }
    if (read_token(token, sizeof(token)) >= 16)
    {
        rt_kprintf("a2a_activate: [ACTIVE] 已激活（token 在 %s），跳过重复激活\n",
                   PKG_AGENT_A2A_TOKEN_FILE);
        if (a2a_kick) rt_sem_release(a2a_kick);
        return 0;
    }
    resolve_endpoint(host, sizeof(host), &port, host_arg, argc > 2 ? port : 0);
    resolve_credentials(username, sizeof(username), password, sizeof(password),
                        user_arg, pass_arg);
    if (!*username || !*password)
    {
        rt_kprintf("a2a_activate: [NO-CRED] 未配置账号密码（menuconfig 的 PKG_AGENT_A2A_USERNAME/"
                   "PASSWORD，或文件 %s）\n", PKG_AGENT_A2A_CRED_FILE);
        rt_kprintf("a2a_activate: 文件写法是一行「账号 密码」，步骤见包 README 的首次激活一节\n");
        return -1;
    }
    if (!a2a_running)
        rt_kprintf("a2a_activate: [INFO] agent 未启动，仅执行激活；启动 main_loop_entry 后自动连接\n");
    return do_activate(host, port, username, password);
}
MSH_CMD_EXPORT(a2a_activate,
               activate device: a2a_activate [host [port [username [password]]]]);

/* 凭 token 连接并进入服务循环。返回时连接已断。 */
static void serve_with_token(const char *host, int port)
{
    char token[257];
    cJSON *hello, *welcome;
    int fd;

    if (read_token(token, sizeof(token)) < 16) return;
    if (agent_ws_random_init())
    {
        rt_kprintf("A2A: 缺少随机种子 /agent_a2a_seed，无法连接\n");
        rt_kprintf("A2A: 先按包 README 的首次激活一节写好种子和凭据，再执行 a2a_activate\n");
        return;
    }
    fd = open_socket(host, port);
    if (fd < 0) return;
    a2a_fd = fd;
    hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "deviceId", PKG_AGENT_A2A_DEVICE_ID);
    cJSON_AddStringToObject(hello, "token", token);
    rt_memset(token, 0, sizeof(token));
    if (send_frame(fd, hello)) { cJSON_Delete(hello); goto disconnected; }
    cJSON_Delete(hello);

    welcome = read_frame(fd);
    if (!welcome)
    {
        rt_kprintf("A2A: 服务端无响应\n");
        goto disconnected;
    }
    if (!strcmp(field(welcome, "type"), "error"))
    {
        const char *code = field(welcome, "code");
        const char *message = field(welcome, "message");
        rt_kprintf("A2A: [REJECTED:%s] 连接被拒绝，%s\n", code, message);
        cJSON_Delete(welcome);
        if (!strcmp(code, "bad_token") || !strcmp(code, "device_not_found"))
        {
            /* 服务端已不认这个 token（设备被删除重建等），回到未激活等重新激活 */
            unlink(PKG_AGENT_A2A_TOKEN_FILE);
            rt_kprintf("A2A: token 已失效并清除，请重新执行 a2a_activate\n");
        }
        goto disconnected;
    }
    if (strcmp(field(welcome, "type"), "welcome"))
    {
        cJSON_Delete(welcome);
        rt_kprintf("A2A: 意外的应答类型\n");
        goto disconnected;
    }
    cJSON_Delete(welcome);
    rt_kprintf("A2A WebSocket connected.\n");

    while (a2a_running)
    {
        fd_set fds;
        struct timeval wait = {1, 0};
        int ready;
        FD_ZERO(&fds); FD_SET(fd, &fds);
        ready = select(fd + 1, &fds, RT_NULL, RT_NULL, &wait);
        if (reports(fd) || ready < 0) break;
        if (!ready) continue;
        {
            cJSON *request = read_frame(fd);
            if (!request) break;
            ready = handle(fd, request);
            cJSON_Delete(request);
            if (ready) break;
        }
    }

disconnected:
    closesocket(fd);
    a2a_fd = -1;
}

static void a2a_worker(void *arg)
{
    char host[64];
    int port;
    rt_bool_t hinted = RT_FALSE;
    (void)arg;

    while (a2a_running)
    {
        char token[257];
        int token_length;

        resolve_endpoint(host, sizeof(host), &port, RT_NULL, 0);
        token_length = read_token(token, sizeof(token));
        rt_memset(token, 0, sizeof(token));

        if (token_length < 16)
        {
            /* 未激活：不自动连接，不每 3 秒重试，阻塞等 a2a_activate 踢醒 */
            if (!hinted)
            {
                rt_kprintf("A2A 未激活 [IDLE]：执行 a2a_activate 激活（设备 ID %s）\n",
                           PKG_AGENT_A2A_DEVICE_ID);
                rt_kprintf("A2A 未激活 [IDLE]：先写好 /agent_a2a_seed 和凭据文件 %s，"
                           "步骤见包 README 的首次激活一节\n", PKG_AGENT_A2A_CRED_FILE);
                hinted = RT_TRUE;
            }
            if (a2a_kick) rt_sem_take(a2a_kick, RT_WAITING_FOREVER);
            continue;
        }
        hinted = RT_FALSE;
        serve_with_token(host, port);
        if (a2a_running) rt_thread_mdelay(3000);
    }
    if (a2a_done) rt_sem_release(a2a_done);
}

static int a2a_init(MessageHub_t hub, Context_t ctx)
{
    (void)ctx;
    if (a2a_running) return RT_EOK;
    a2a_hub = hub;
    a2a_done = rt_sem_create("a2adone", 0, RT_IPC_FLAG_FIFO);
    a2a_kick = rt_sem_create("a2akick", 0, RT_IPC_FLAG_FIFO);
    if (!a2a_done || !a2a_kick)
    {
        if (a2a_done) rt_sem_delete(a2a_done);
        if (a2a_kick) rt_sem_delete(a2a_kick);
        a2a_done = a2a_kick = RT_NULL;
        return -RT_ENOMEM;
    }
    a2a_thread = rt_thread_create("agenta2a", a2a_worker, RT_NULL, 8192, 15, 10);
    if (!a2a_thread)
    {
        rt_sem_delete(a2a_done);
        rt_sem_delete(a2a_kick);
        a2a_done = a2a_kick = RT_NULL;
        return -RT_ENOMEM;
    }
    a2a_running = RT_TRUE;
    rt_thread_startup(a2a_thread);
    return RT_EOK;
}

static void a2a_reset(void)
{
    if (!a2a_done) return;
    a2a_running = RT_FALSE;
    if (a2a_fd >= 0) shutdown(a2a_fd, SHUT_RDWR);
    if (a2a_kick) rt_sem_release(a2a_kick);
    rt_sem_take(a2a_done, RT_WAITING_FOREVER);
    rt_sem_delete(a2a_done);
    rt_sem_delete(a2a_kick);
    a2a_done = a2a_kick = RT_NULL;
    a2a_thread = RT_NULL;
    a2a_hub = RT_NULL;
    if (bound_card) { cJSON_Delete(bound_card); bound_card = RT_NULL; }
}
AgentChannelOps agent_a2a_ops = {a2a_init, a2a_reset};
