#include "utils/utils_internal.h"
#include "dfs_posix.h"
#include "unistd.h"

#define LOG_TAG "Agent.utils.format"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

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
