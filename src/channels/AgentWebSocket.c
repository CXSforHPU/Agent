#include "AgentWebSocket.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <dfs_posix.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <mbedtls/hmac_drbg.h>

static mbedtls_hmac_drbg_context rng;
static int rng_ready;

int agent_ws_random_init(void)
{
    unsigned char seed[48];
    int fd, n;
    if (!rng_ready)
    {
        fd = open("/agent_a2a_seed", O_RDONLY);
        if (fd < 0) return -1;
        n = read(fd, seed, sizeof(seed)); close(fd);
        if (n != sizeof(seed)) return -1;
        mbedtls_hmac_drbg_init(&rng);
        if (mbedtls_hmac_drbg_seed_buf(&rng, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), seed, sizeof(seed)))
            return -1;
        rng_ready = 1;
    }
    /* Persist the next seed BEFORE using this generator on the wire. Never use
     * the package's rand()-based entropy shim for WebSocket client masking. */
    if (mbedtls_hmac_drbg_random(&rng, seed, sizeof(seed))) return -1;
    fd = open("/agent_a2a_seed", O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    n = write(fd, seed, sizeof(seed)); close(fd);
    memset(seed, 0, sizeof(seed));
    return n == sizeof(seed) ? 0 : -1;
}

static int transfer(int fd, void *buffer, int size, int writing)
{
    int offset = 0;
    rt_tick_t start = rt_tick_get();
    while (offset < size && rt_tick_get() - start < rt_tick_from_millisecond(10000))
    {
        fd_set fds;
        struct timeval wait = {0, 200000};
        int n;
        FD_ZERO(&fds); FD_SET(fd, &fds);
        n = select(fd + 1, writing ? RT_NULL : &fds, writing ? &fds : RT_NULL, RT_NULL, &wait);
        if (n < 0) return -1;
        if (!n) continue;
        n = writing ? send(fd, (char *)buffer + offset, size - offset, 0)
                    : recv(fd, (char *)buffer + offset, size - offset, 0);
        if (n <= 0) return -1;
        offset += n;
    }
    return offset == size ? 0 : -1;
}

int agent_ws_send(int fd, unsigned char opcode, const void *bytes, int size)
{
    unsigned char header[8], mask[4], block[256];
    int length = 2, offset = 0, i, n;
    if (size < 0 || size > 8192 || (opcode >= 8 && size > 125)) return -1;
    if (mbedtls_hmac_drbg_random(&rng, mask, sizeof(mask))) return -1;
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | (size < 126 ? size : 126);
    if (size >= 126) { header[2] = size >> 8; header[3] = size; length = 4; }
    memcpy(header + length, mask, 4); length += 4;
    if (transfer(fd, header, length, 1)) return -1;
    while (offset < size)
    {
        n = size - offset; if (n > sizeof(block)) n = sizeof(block);
        for (i = 0; i < n; ++i) block[i] = ((const unsigned char *)bytes)[offset + i] ^ mask[(offset + i) & 3];
        if (transfer(fd, block, n, 1)) return -1;
        offset += n;
    }
    return 0;
}

static int valid_utf8(const unsigned char *p, int n)
{
    int i = 0;
    while (i < n)
    {
        uint32_t c = p[i++], min;
        int extra;
        if (c < 0x80) { if (!c) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; c &= 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; c &= 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; c &= 7; min = 0x10000; }
        else return 0;
        if (i + extra > n) return 0;
        while (extra--) { if ((p[i] & 0xc0) != 0x80) return 0; c = (c << 6) | (p[i++] & 63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}

int agent_ws_read(int fd, char *bytes, int capacity)
{
    int used = 0, fragmented = 0;
    rt_tick_t start = rt_tick_get();
    for (;;)
    {
        unsigned char h[2], control[125];
        int opcode, fin, size;
        if (rt_tick_get() - start > rt_tick_from_millisecond(10000)) return -1;
        if (transfer(fd, h, 2, 0)) return -1;
        fin = h[0] & 128; opcode = h[0] & 15; size = h[1] & 127;
        if ((h[0] & 0x70) || (h[1] & 128) || size == 127) return -1;
        if (size == 126)
        {
            if (transfer(fd, h, 2, 0)) return -1;
            size = h[0] * 256 + h[1]; if (size < 126) return -1;
        }
        if (opcode >= 8)
        {
            if (!fin || size > 125 || (opcode != 8 && opcode != 9 && opcode != 10)) return -1;
            if (transfer(fd, control, size, 0)) return -1;
            if (opcode == 8)
            {
                if (size == 1) return -1;
                agent_ws_send(fd, 8, control, size); return -1;
            }
            if (opcode == 9 && agent_ws_send(fd, 10, control, size)) return -1;
            if (!fragmented) return -2; /* control frame only */
            continue;
        }
        if ((opcode != 1 && opcode != 0) || (opcode == 0) != fragmented || size > capacity - 1 - used)
            return -1;
        if (transfer(fd, bytes + used, size, 0)) return -1;
        used += size;
        if (fin)
        {
            bytes[used] = 0;
            return valid_utf8((unsigned char *)bytes, used) ? used : -1;
        }
        fragmented = 1;
    }
}

int agent_ws_upgrade(int fd, const char *host, const char *path)
{
    unsigned char nonce[16], digest[20];
    /* 768 而不是 2048：本函数会在调用者栈上开这块缓冲，MSH 的 tShell 只有
       几 KB，2 KB 的响应缓冲加调用链曾把 shell 线程撑爆（栈溢出后
       rt_schedule 里的栈检查打日志又在调度器内拿 ulog 锁，直接断言）。
       升级响应头只有两百字节，768 足够；超了返回 -1 而不是越界写。 */
    char key[32], expected[32], hash_input[80], request[512], response[768];
    char *line, *next;
    size_t n;
    int used = 0, accept = 0, upgrade = 0, connection = 0;
    rt_tick_t start = rt_tick_get();
    if (mbedtls_hmac_drbg_random(&rng, nonce, sizeof(nonce)) ||
        mbedtls_base64_encode((unsigned char *)key, sizeof(key), &n, nonce, sizeof(nonce))) return -1;
    key[n] = 0;
    rt_snprintf(hash_input, sizeof(hash_input), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    mbedtls_sha1((unsigned char *)hash_input, strlen(hash_input), digest);
    mbedtls_base64_encode((unsigned char *)expected, sizeof(expected), &n, digest, sizeof(digest)); expected[n] = 0;
    n = rt_snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n", path, host, key);
    if (n >= sizeof(request) || transfer(fd, request, n, 1)) return -1;
    /* Byte reads intentionally leave any coalesced first frame in the socket. */
    while (used < sizeof(response) - 1)
    {
        if (rt_tick_get() - start > rt_tick_from_millisecond(10000) || transfer(fd, response + used, 1, 0)) return -1;
        response[++used] = 0;
        if (used >= 4 && !memcmp(response + used - 4, "\r\n\r\n", 4)) break;
    }
    if (used == sizeof(response) - 1 || strncmp(response, "HTTP/1.1 101 ", 13)) return -1;
    line = strstr(response, "\r\n") + 2;
    while ((next = strstr(line, "\r\n")) != RT_NULL && next != line)
    {
        char *colon;
        *next = 0; colon = strchr(line, ':');
        if (!colon) return -1;
        *colon++ = 0; while (*colon == ' ' || *colon == '\t') ++colon;
        if (!strcasecmp(line, "Sec-WebSocket-Accept")) { if (accept || strcmp(colon, expected)) return -1; accept = 1; }
        if (!strcasecmp(line, "Upgrade") && !strcasecmp(colon, "websocket")) upgrade = 1;
        if (!strcasecmp(line, "Connection") && !strcasecmp(colon, "upgrade")) connection = 1;
        line = next + 2;
    }
    return accept && upgrade && connection ? 0 : -1;
}
