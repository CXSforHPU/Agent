#ifndef __AGENT_UTILS_H__
#define __AGENT_UTILS_H__
#include "rtconfig.h"
#include <rtthread.h>
#include "cJSON.h"
#include "webclient.h"
#include "string.h"
#include "MessageHub.h"

#define LOG_TAG "Agent.utils"
#define LOG_LVL LOG_LVL_INFO
#include <ulog.h>

/*
 * @brief 格式化文本为 cJSON 对象 { type, text }
 * @param text 文本内容
 * @return cJSON 对象指针
 */
cJSON *format_text(const char *text);

/*
 * @brief 创建空内容数组
 * @return cJSON 数组指针
 */
cJSON *create_content_array(void);

/*
 * @brief 将 Messages 转换为 API 要求的 content 格式
 * @param messages 消息数组
 * @return 非多模态返回 {content:xxx}，多模态返回 [{type, ...}]
 */
cJSON *to_content(Messages_t messages);

/* 回调打印函数 */
void print_reasoning(const char *text);
void print_tool_call(const char *text);
void print_context(const char *text);

/*
 * @brief 创建工具项对象
 * @param func_name  函数名
 * @param desc       函数描述
 * @param params_obj 参数对象
 * @return 工具项 cJSON 对象
 */
cJSON *create_tool_item(
    const char *func_name,
    const char *desc,
    cJSON *params_obj);

/*
 * @brief 创建参数对象 { type, properties, required }
 * @param props_obj    属性对象
 * @param required_arr 必填数组
 * @return 参数 cJSON 对象
 */
cJSON *create_param_obj(
    cJSON *props_obj,
    cJSON *required_arr);

/*
 * @brief 创建单个属性并添加到属性对象
 * @param props_obj 属性对象
 * @param name      属性名
 * @param type      属性类型
 * @param desc      属性描述
 */
void create_property(cJSON *props_obj, const char *name, const char *type, const char *desc);

/* 文件服务器 - 多模态 */
#ifdef PKG_AGENT_MULTIMODAL_ENABLE
typedef enum
{
    CMD_AGENT_FILE_UPLOAD,
    CMD_AGENT_FILE_DOWNLOAD,
    CMD_AGENT_FILE_EXIT,
    CMD_AGENT_FILE_URL
} file_op_type_t;

typedef struct
{
    file_op_type_t op;
    char path[128];
    char file_id[128];
} file_op_req;

rt_thread_t agent_file_op_init(void);
void agent_file_op_deinit(void);
char *agent_up_load(const char *path);
rt_err_t down_load(const char *path, const char *file_id);

#endif

#endif // __AGENT_UTILS_H__