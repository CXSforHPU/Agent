#ifndef __AGENT_UTILS_FILE_OP_H__
#define __AGENT_UTILS_FILE_OP_H__

#include "rtconfig.h"

/* 文件服务器 - 多模态 */
#ifdef PKG_AGENT_MULTIMODAL_ENABLE

/* multipart 上传的收发缓冲，文件内容按这个大小分片送出 */
#define AGENT_FILE_OP_IO_BUF_SZ   1024
/* 大文件传得慢，连上之后把收发超时从 webclient 默认的 6 s 放宽到这里 */
#define AGENT_FILE_OP_TIMEOUT_MS  120000

typedef enum
{
    CMD_AGENT_FILE_UPLOAD,
    CMD_AGENT_FILE_DOWNLOAD,
    CMD_AGENT_FILE_EXIT,
    CMD_AGENT_FILE_EXIT_COMPLETE,
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

/* 上传同步等待返回 URL；下载只入队，由 fileop_wk 线程执行 */
char *agent_up_load(const char *path);
rt_err_t agent_down_load(const char *path, const char *file_id);

/* 文件操作线程收发的两个队列 */
rt_mq_t get_agent_file_op_mq_input(void);
rt_mq_t get_agent_file_op_mq_output(void);

#endif // PKG_AGENT_MULTIMODAL_ENABLE

#endif // __AGENT_UTILS_FILE_OP_H__
