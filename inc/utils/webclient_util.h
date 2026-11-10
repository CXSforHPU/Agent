#ifndef __AGENT_UTILS_WEBCLIENT_UTIL_H__
#define __AGENT_UTILS_WEBCLIENT_UTIL_H__

#include <rtthread.h>
#include <webclient.h>

/*
 * webclient v2.2.0 的超时接口有两个坑，包内统一走这里的封装：
 *
 *   1. webclient_set_timeout() 把超时设到 session->socket 上。连接建立前
 *      socket 还是 -1，设置会被丢掉；connect() 之后又会用写死的 6 秒覆盖
 *      一次。所以它必须在连接成功之后调用。
 *   2. 它把毫秒换算成秒时算的是 rt_tick_from_millisecond(ms) / 1000，
 *      分母写死成 1000，等于假设 RT_TICK_PER_SECOND 是 1000。本工程是 100，
 *      请求的毫秒数会被截掉 10 倍，15000ms 实际只设成 1 秒。
 *
 * 下面的接口按毫秒给出超时，内部换算成 webclient 真正设得出的秒数；
 * socket 还没连上时直接返回，什么都不做。
 */

/* webclient v2.2.0 里这个函数不是 static，但没有写进头文件 */
int webclient_handle_response(struct webclient_session *session);

/*
 * @brief 把会话的收发超时设成指定毫秒数
 * @param session     会话，socket 必须已经连上
 * @param millisecond 超时毫秒数，不足 1000ms 的部分向上取整到 1 秒
 */
void agent_webclient_set_timeout(struct webclient_session *session, int millisecond);

#endif /* __AGENT_UTILS_WEBCLIENT_UTIL_H__ */
