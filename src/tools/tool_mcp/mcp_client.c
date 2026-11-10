/*
 * MCP 客户端：JSON-RPC over HTTP，负责握手、拉取远端工具表与调用远端工具
 * 约束：会话 id 与 RPC id 是全局状态，断开后必须复位，否则下次握手会被服务端拒绝。
 */
#include "mcp_client.h"

#ifdef PKG_AGENT_TOOL_MCP_ENABLE

#include <webclient.h>
#include "utils/webclient_util.h"

#define LOG_TAG "Agent.mcp"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

#define MCP_PROTOCOL_VERSION "2025-06-18"
#define MCP_HEADER_BUFSZ     1024
#define MCP_SESSION_ID_MAX   64

static char session_id[MCP_SESSION_ID_MAX];
static int rpc_id;
static cJSON *remote_tools;
static rt_bool_t connected = RT_FALSE;

/*
 * @brief 读取完整响应体
 * @param session  webclient 会话
 * @param out_len 输出 body 长度
 * @return rt_malloc 分配的缓冲区（末尾补 '\0'），失败返回 NULL
 * @note 超过 PKG_AGENT_MCP_RESP_BUFSZ 视为失败，避免无界增长
 */
static char *read_body(struct webclient_session *session, int *out_len)
{
    char *buf = (char *)rt_malloc(PKG_AGENT_MCP_RESP_BUFSZ + 1);
    int total = 0;
    int n;

    if (!buf)
    {
        return RT_NULL;
    }
    while ((n = webclient_read(session, buf + total, PKG_AGENT_MCP_RESP_BUFSZ - total)) > 0)
    {
        total += n;
        if (total >= PKG_AGENT_MCP_RESP_BUFSZ)
        {
            LOG_E("mcp: response exceeds %d bytes, raise PKG_AGENT_MCP_RESP_BUFSZ",
                  PKG_AGENT_MCP_RESP_BUFSZ);
            rt_free(buf);
            return RT_NULL;
        }
    }
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

/*
 * @brief 记录响应头里的会话 ID
 * @param session webclient 会话
 * @note header_fields_get 返回的串尾带 \r\n，逐字符截断拷贝
 */
static void save_session_id(struct webclient_session *session)
{
    const char *value = webclient_header_fields_get(session, "Mcp-Session-Id");
    int i;

    if (!value || !value[0])
    {
        return;
    }
    for (i = 0; i < MCP_SESSION_ID_MAX - 1; i++)
    {
        char c = value[i];
        if (c == '\0' || c == '\r' || c == '\n')
        {
            break;
        }
        session_id[i] = c;
    }
    session_id[i] = '\0';
}

/*
 * @brief 从响应体里取出匹配请求 id 的 JSON-RPC 报文
 * @param body 响应体，兼容纯 JSON 与 SSE（data: 行）两种格式
 * @param id  请求 id
 * @return 解析出的 cJSON 对象，调用方释放；找不到返回 NULL
 */
static cJSON *parse_rpc_response(const char *body, int id)
{
    cJSON *fallback = RT_NULL;
    const char *p = body;

    if (body[0] == '{')
    {
        return cJSON_Parse(body);
    }

    while (*p)
    {
        const char *eol = p;
        int len;
        const char *data = RT_NULL;
        int data_len = 0;
        cJSON *json;
        cJSON *id_item;

        while (*eol && *eol != '\n')
        {
            eol++;
        }
        len = (int)(eol - p);

        if (len > 0 && p[len - 1] == '\r')
        {
            len--;
        }
        if (len > 5 && rt_strncmp(p, "data:", 5) == 0)
        {
            data = p + 5;
            data_len = len - 5;
            if (data_len > 0 && *data == ' ')
            {
                data++;
                data_len--;
            }
        }
        p = eol + 1;
        if (!data || data_len == 0)
        {
            continue;
        }
        json = cJSON_ParseWithLength(data, data_len);
        if (!json)
        {
            continue;
        }
        id_item = cJSON_GetObjectItemCaseSensitive(json, "id");
        if (cJSON_IsNumber(id_item) && id_item->valueint == id)
        {
            cJSON_Delete(fallback);
            return json;
        }
        if (!fallback && (cJSON_GetObjectItemCaseSensitive(json, "result") ||
                          cJSON_GetObjectItemCaseSensitive(json, "error")))
        {
            fallback = json;
        }
        else
        {
            cJSON_Delete(json);
        }
    }
    return fallback;
}

/*
 * @brief 发送一条 JSON-RPC 报文并取回响应对象
 * @param payload 序列化后的请求体
 * @param id      请求 id，负值表示通知（不期待响应体）
 * @param resp    输出响应 JSON，由调用方释放；传 NULL 表示丢弃
 * @return RT_EOK 成功，负值失败
 */
static rt_err_t mcp_transact(const char *payload, int id, cJSON **resp)
{
    struct webclient_session *session;
    int status;
    int len = 0;
    char *body = RT_NULL;
    cJSON *json = RT_NULL;
    rt_err_t ret = -RT_ERROR;
    int payload_len = (int)rt_strlen(payload);

    session = webclient_session_create(MCP_HEADER_BUFSZ);
    if (!session)
    {
        return -RT_ENOMEM;
    }
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Accept: application/json, text/event-stream\r\n");
    if (session_id[0])
    {
        webclient_header_fields_add(session, "Mcp-Session-Id: %s\r\n", session_id);
    }
    webclient_header_fields_add(session, "Content-Length: %d\r\n", payload_len);

    /*
     * 先只连接并把请求头发出去。远端工具算得慢，截屏要好几秒，响应头要等
     * 工具算完才回来，这段等待必须算在超时里。而 webclient 的超时只能设在
     * 已经连上的 socket 上，连之前设会被丢掉，所以这里分成两段：
     * 连接 → 设超时 → 发请求体 → 收响应。
     */
    status = webclient_post(session, PKG_AGENT_MCP_SERVER_URL, RT_NULL, 0);
    if (status < 0)
    {
        LOG_E("mcp: cannot reach %s, status %d", PKG_AGENT_MCP_SERVER_URL, status);
        goto __exit;
    }
    agent_webclient_set_timeout(session, PKG_AGENT_MCP_TIMEOUT_MS);

    if (webclient_write(session, payload, payload_len) != payload_len)
    {
        LOG_E("mcp: cannot send the request body");
        goto __exit;
    }
    status = webclient_handle_response(session);
    if (status == 202 || status == 204)
    {
        /* 通知被接受，无响应体 */
        ret = RT_EOK;
        goto __exit;
    }
    if (status != 200)
    {
        LOG_E("mcp: HTTP status %d from %s", status, PKG_AGENT_MCP_SERVER_URL);
        goto __exit;
    }
    save_session_id(session);

    body = read_body(session, &len);
    if (!body)
    {
        goto __exit;
    }
    if (len > 0)
    {
        json = parse_rpc_response(body, id);
    }
    rt_free(body);
    if (id >= 0 && !json)
    {
        LOG_E("mcp: no JSON-RPC response for id %d", id);
        goto __exit;
    }
    ret = RT_EOK;

__exit:
    webclient_close(session);
    if (resp)
    {
        *resp = json;
    }
    else
    {
        cJSON_Delete(json);
    }
    return ret;
}

/*
 * @brief 发送 JSON-RPC 请求并取回 result
 * @param method  方法名
 * @param params  参数对象，所有权交给本函数，失败路径也会释放
 * @param result  输出 result 对象（从响应里摘出），调用方释放
 * @param err_msg 可选输出：服务端 error.message 的拷贝，调用方释放
 * @return RT_EOK 成功，负值失败
 */
static rt_err_t mcp_request(const char *method, cJSON *params, cJSON **result, char **err_msg)
{
    cJSON *req = cJSON_CreateObject();
    cJSON *resp = RT_NULL;
    cJSON *err;
    char *payload;
    rt_err_t ret;
    int id = ++rpc_id;

    if (result)
    {
        *result = RT_NULL;
    }
    if (err_msg)
    {
        *err_msg = RT_NULL;
    }
    if (!req)
    {
        cJSON_Delete(params);
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    if (params)
    {
        cJSON_AddItemToObject(req, "params", params);
    }
    payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!payload)
    {
        return -RT_ENOMEM;
    }
    ret = mcp_transact(payload, id, &resp);
    rt_free(payload);
    if (ret != RT_EOK)
    {
        cJSON_Delete(resp);
        return ret;
    }

    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    if (err)
    {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        const char *msg_str = cJSON_IsString(msg) ? msg->valuestring : "unknown error";

        LOG_E("mcp: %s failed, code %d, %s", method,
              cJSON_IsNumber(code) ? code->valueint : 0, msg_str);
        if (err_msg)
        {
            *err_msg = rt_strdup(msg_str);
        }
        cJSON_Delete(resp);
        return -RT_ERROR;
    }
    if (result)
    {
        *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
    }
    cJSON_Delete(resp);
    return RT_EOK;
}

/*
 * @brief 发送 JSON-RPC 通知（无 id，不期待响应体）
 * @param method 方法名
 * @return RT_EOK 成功，负值失败
 */
static rt_err_t mcp_notify(const char *method)
{
    cJSON *req = cJSON_CreateObject();
    char *payload;
    rt_err_t ret;

    if (!req)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!payload)
    {
        return -RT_ENOMEM;
    }
    ret = mcp_transact(payload, -1, RT_NULL);
    rt_free(payload);
    return ret;
}

/*
 * @brief 连接 MCP 服务端并拉取远端工具表
 * @return RT_EOK 成功，负值失败
 */
rt_err_t mcp_client_connect(void)
{
    cJSON *params;
    cJSON *info;
    cJSON *result = RT_NULL;
    cJSON *tools;

    if (connected)
    {
        return RT_EOK;
    }
    mcp_client_disconnect();

    params = cJSON_CreateObject();
    if (!params)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    info = cJSON_CreateObject();
    if (!info)
    {
        cJSON_Delete(params);
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(info, "name", "rt-thread-agent");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddItemToObject(params, "clientInfo", info);

    if (mcp_request("initialize", params, &result, RT_NULL) != RT_EOK)
    {
        return -RT_ERROR;
    }
    cJSON_Delete(result);
    result = RT_NULL;

    if (mcp_notify("notifications/initialized") != RT_EOK)
    {
        mcp_client_disconnect();
        return -RT_ERROR;
    }

    if (mcp_request("tools/list", RT_NULL, &result, RT_NULL) != RT_EOK)
    {
        mcp_client_disconnect();
        return -RT_ERROR;
    }
    tools = result ? cJSON_DetachItemFromObjectCaseSensitive(result, "tools") : RT_NULL;
    cJSON_Delete(result);
    if (!cJSON_IsArray(tools))
    {
        cJSON_Delete(tools);
        LOG_E("mcp: tools/list returned no tools array");
        mcp_client_disconnect();
        return -RT_ERROR;
    }
    remote_tools = tools;
    connected = RT_TRUE;
    LOG_I("mcp: connected to %s, %d remote tools", PKG_AGENT_MCP_SERVER_URL,
          cJSON_GetArraySize(tools));
    return RT_EOK;
}

/*
 * @brief 取回已连接的远端工具表
 * @return 工具表数组，未连接时返回 NULL
 */
cJSON *mcp_client_tools(void)
{
    return remote_tools;
}

/*
 * @brief 把媒体条目的地址拼成一行文本
 * @param buf  输出缓冲
 * @param size 缓冲大小
 * @param item content 里的一个条目
 * @param kind 媒体类型，image 或 audio
 * @return buf 里的文本。条目没有地址时给一句占位说明
 */
static const char *media_to_text(char *buf, rt_size_t size, cJSON *item, const char *kind)
{
    cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");

    if (cJSON_IsString(url) && url->valuestring && url->valuestring[0])
    {
        /* 网关把图片和音频存到了文件服务器，回包里带的是下载地址 */
        rt_snprintf(buf, size, "[%s: %s]", kind, url->valuestring);
    }
    else
    {
        rt_snprintf(buf, size, "[%s: this board shows text only]", kind);
    }
    return buf;
}

/*
 * @brief 把 tools/call 结果里的 text 内容拼成一段文本
 * @param result tools/call 的 result 对象
 * @return rt_malloc 分配的文本，调用方释放；无文本时给出占位说明
 * @note 总长封顶 PKG_AGENT_MCP_RESP_BUFSZ，超长截断。图片和音频换成文件服务器的地址
 */
static char *result_to_text(cJSON *result)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    cJSON *item;
    int cap = PKG_AGENT_MCP_RESP_BUFSZ;
    int used = 0;
    char *text = (char *)rt_malloc(cap);

    if (!text)
    {
        return RT_NULL;
    }
    text[0] = '\0';
    cJSON_ArrayForEach(item, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        cJSON *txt = cJSON_GetObjectItemCaseSensitive(item, "text");
        const char *part;
        char media[256];
        int len;
        int copy;

        if (!cJSON_IsString(type))
        {
            continue;
        }
        if (rt_strcmp(type->valuestring, "text") == 0)
        {
            if (!cJSON_IsString(txt))
            {
                continue;
            }
            part = txt->valuestring;
        }
        else if (rt_strcmp(type->valuestring, "image") == 0)
        {
            part = media_to_text(media, sizeof(media), item, "image");
        }
        else if (rt_strcmp(type->valuestring, "audio") == 0)
        {
            part = media_to_text(media, sizeof(media), item, "audio");
        }
        else
        {
            continue;
        }
        len = (int)rt_strlen(part);
        if (used > 0 && used < cap - 1)
        {
            text[used++] = '\n';
        }
        copy = len;
        if (copy > cap - 1 - used)
        {
            copy = cap - 1 - used;
        }
        if (copy > 0)
        {
            rt_memcpy(text + used, part, copy);
            used += copy;
        }
        if (used >= cap - 1)
        {
            break;
        }
    }
    text[used] = '\0';
    if (used == 0)
    {
        rt_strncpy(text, "(MCP tool returned no text content)", cap - 1);
    }
    return text;
}

/*
 * @brief 调用远端 MCP 工具
 * @param name      远端工具名
 * @param arguments 工具参数对象
 * @param out_text  输出结果文本（可为 NULL），调用方释放
 * @return RT_EOK 成功，负值失败
 */
rt_err_t mcp_client_call(const char *name, cJSON *arguments, char **out_text)
{
    cJSON *params;
    cJSON *result = RT_NULL;
    char *err_msg = RT_NULL;
    rt_err_t ret;
    rt_bool_t is_error;

    if (out_text)
    {
        *out_text = RT_NULL;
    }
    if (!name || !name[0])
    {
        return -RT_EINVAL;
    }
    if (!connected && mcp_client_connect() != RT_EOK)
    {
        if (out_text)
        {
            *out_text = rt_strdup("MCP server is not connected.");
        }
        return -RT_ERROR;
    }

    params = cJSON_CreateObject();
    if (!params)
    {
        return -RT_ENOMEM;
    }
    cJSON_AddStringToObject(params, "name", name);
    cJSON_AddItemToObject(params, "arguments",
                          arguments ? cJSON_Duplicate(arguments, cJSON_True) : cJSON_CreateObject());

    ret = mcp_request("tools/call", params, &result, &err_msg);
    if (ret != RT_EOK)
    {
        if (out_text)
        {
            *out_text = err_msg ? err_msg : rt_strdup("MCP tools/call failed.");
        }
        else
        {
            rt_free(err_msg);
        }
        cJSON_Delete(result);
        return ret;
    }
    rt_free(err_msg);

    is_error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError"));
    if (out_text)
    {
        *out_text = result_to_text(result);
    }
    cJSON_Delete(result);
    return is_error ? -RT_ERROR : RT_EOK;
}

/*
 * @brief 断开 MCP 客户端并清空会话与工具表
 */
void mcp_client_disconnect(void)
{
    connected = RT_FALSE;
    session_id[0] = '\0';
    rpc_id = 0;
    if (remote_tools)
    {
        cJSON_Delete(remote_tools);
        remote_tools = RT_NULL;
    }
}

#endif /* PKG_AGENT_TOOL_MCP_ENABLE */