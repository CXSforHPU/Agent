#ifndef AGENT_REPORT_H
#define AGENT_REPORT_H
#include <rtthread.h>
#define AGENT_REPORT_TEXT_MAX 512
#define AGENT_REPORT_QUEUE_SIZE 4
void agent_report_init(void);
void agent_report_deinit(void);
rt_err_t agent_report_push(const char *text);
/* A2A worker drains this bounded queue over its authenticated WebSocket. */
rt_err_t agent_report_take(char *text, rt_size_t size);
rt_bool_t agent_report_ready(void);
#endif
