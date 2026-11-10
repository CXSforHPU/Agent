#include "rtconfig.h"
#include "utils/webclient_util.h"

/*
 * @brief 把会话的收发超时设成指定毫秒数
 * @param session     会话，socket 必须已经连上
 * @param millisecond 超时毫秒数
 * @note 传给 webclient 的毫秒数要放大 1000 * 1000 / RT_TICK_PER_SECOND 倍，
 *       它内部算的是 rt_tick_from_millisecond(ms) / 1000，分母写死成 1000。
 */
void agent_webclient_set_timeout(struct webclient_session *session, int millisecond)
{
    int second;

    if (!session || session->socket < 0)
    {
        /* 还没连上，超时设了也没用 */
        return;
    }
    second = (millisecond + 999) / 1000;
    webclient_set_timeout(session, (int)((long long)second * 1000000 / RT_TICK_PER_SECOND));
}
