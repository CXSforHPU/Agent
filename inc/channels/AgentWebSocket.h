#ifndef AGENT_WEBSOCKET_H
#define AGENT_WEBSOCKET_H
#include <rtthread.h>
/* RFC 6455 client. Plain ws only; reject wss rather than bypass TLS verification.
 * Provision 48 random bytes at /agent_a2a_seed. Rotated before each connection. */
int agent_ws_random_init(void);
int agent_ws_upgrade(int fd, const char *host, const char *path);
int agent_ws_send(int fd, unsigned char opcode, const void *bytes, int size);
/* Reads one complete UTF-8 text message; handles continuation and control frames. */
int agent_ws_read(int fd, char *bytes, int capacity);
#endif
