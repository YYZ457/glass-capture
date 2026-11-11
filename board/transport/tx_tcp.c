/* SPDX-License-Identifier: MIT
 *
 * tx_tcp.c —— I6 协议的板端发送。
 *
 * 每条消息 = 32 字节头 + 负载，全部小端：
 *   0 "GCAP"   4 u16 版本=1   6 u16 类型   8 u64 序号   16 u32 block_id
 *   20 u32 负载字节数   24 u32 负载 CRC32（和 Python zlib.crc32 相同）   28 u32 保留=0
 * 类型 1 原图块：u32 width, u32 height, width×height 字节
 * 类型 2 候选：  u32 count, u32 flags（bit0 = 截断）, 再 count × 32 字节（x0 y0 x1 y1 area peak 各 u32，cx cy 各 f32）
 * 类型 3 状态：  一行 JSON（不带换行）
 *
 * 怎么验证：电脑上跑 rx_tcp.py，发一块已知图，收到的 PGM 的 SHA256 一致。
 * 状态：🟡 电脑上本机回环测过
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "tx_tcp.h"
#include "sg_err.h"

#define HEADER_SIZE   32
#define VERSION       1
#define TYPE_FRAME    1
#define TYPE_CANDS    2
#define TYPE_STATUS   3
#define CAND_SIZE     32

static uint64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* ==== 小工具：小端写入、CRC32 ========================================== */

static void put16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* 标准 CRC32（IEEE，反射多项式 0xEDB88320）。crc 初值传 0，可以分段累加。 */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    const unsigned char *p = data;

    if (table[1] == 0) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) {
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ==== 发送 ============================================================= */

/* 在 deadline 之前把 n 字节全部发出去。 */
static int send_all(int fd, const void *data, size_t n, uint64_t deadline)
{
    const unsigned char *p = data;

    while (n) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        uint64_t now = now_ms();
        ssize_t sent;
        int ready;

        if (now >= deadline) {
            return -ETIMEDOUT;
        }
        ready = poll(&pfd, 1, (int)(deadline - now));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (ready == 0) {
            return -ETIMEDOUT;
        }
        sent = send(fd, p, n, MSG_NOSIGNAL);    /* 对方断开时返回 EPIPE，而不是让进程收到 SIGPIPE */
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -errno;
        }
        p += sent;
        n -= (size_t)sent;
    }
    return 0;
}

/* 发一条消息：负载最多分两段（前缀 + 主体），CRC 覆盖两段。 */
static int send_message(struct tx_tcp *t, uint16_t type, uint32_t block_id,
                        const void *a, size_t na, const void *b, size_t nb, uint32_t timeout_ms)
{
    unsigned char header[HEADER_SIZE];
    uint64_t deadline = now_ms() + timeout_ms;
    uint32_t crc;
    int ret;

    if (t == NULL || t->fd < 0 || timeout_ms == 0) {
        return -EINVAL;
    }
    if (na + nb > UINT32_MAX) {
        return -EMSGSIZE;
    }
    crc = crc32_update(0, a, na);
    crc = crc32_update(crc, b, nb);

    memcpy(header, "GCAP", 4);
    put16(header + 4, VERSION);
    put16(header + 6, type);
    put64(header + 8, ++t->sequence);
    put32(header + 16, block_id);
    put32(header + 20, (uint32_t)(na + nb));
    put32(header + 24, crc);
    put32(header + 28, 0);

    ret = send_all(t->fd, header, sizeof(header), deadline);
    if (!ret && na) {
        ret = send_all(t->fd, a, na, deadline);
    }
    if (!ret && nb) {
        ret = send_all(t->fd, b, nb, deadline);
    }
    return ret;
}

/* ==== 连接 ============================================================= */

/* 连接电脑。host 只接受数字 IPv4 地址（如 "192.168.1.10"）：
 * 板上程序是静态链接的，用 getaddrinfo 解析主机名需要运行时的 glibc 动态库，板上不一定有。
 * 用非阻塞 connect + poll，才能给连接加超时。 */
int tx_tcp_connect(struct tx_tcp *t, const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    socklen_t len = sizeof(int);
    int fd, err = 0, ready;

    if (t == NULL) {
        return -EINVAL;
    }
    t->fd = -1;
    t->sequence = 0;
    if (host == NULL || port == 0 || timeout_ms == 0) {
        return -EINVAL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return -EINVAL;
    }

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -errno;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) && errno != EINPROGRESS) {
        err = -errno;
        close(fd);
        return err;
    }

    /* 等连接完成：可写 = 连上了或者失败了，再用 SO_ERROR 区分 */
    pfd.fd = fd;
    pfd.events = POLLOUT;
    do {
        ready = poll(&pfd, 1, (int)timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        err = ready == 0 ? -ETIMEDOUT : -errno;
        close(fd);
        return err;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
        close(fd);
        return err ? -err : -EIO;
    }
    t->fd = fd;
    return 0;
}

void tx_tcp_close(struct tx_tcp *t)
{
    if (t != NULL && t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

/* ==== 三种消息 ========================================================= */

static int send_pixels(struct tx_tcp *t, uint32_t block_id, const unsigned char *pixels,
                       uint32_t width, uint32_t height, uint32_t timeout_ms)
{
    unsigned char prefix[8];

    put32(prefix, width);
    put32(prefix + 4, height);
    return send_message(t, TYPE_FRAME, block_id, prefix, sizeof(prefix),
                        pixels, (size_t)width * height, timeout_ms);
}

int tx_tcp_send_frame(struct tx_tcp *t, const struct sg_capture_frame *f, uint32_t timeout_ms)
{
    if (f == NULL || f->data == NULL || f->width == 0 || f->height == 0 ||
        (uint64_t)f->width * f->height != f->length) {
        return -EINVAL;
    }
    return send_pixels(t, f->block_id, f->data, f->width, f->height, timeout_ms);
}

int tx_tcp_send_candidates(struct tx_tcp *t, uint32_t block_id, const struct sg_candidate *c,
                           uint32_t count, int truncated, uint32_t timeout_ms)
{
    unsigned char prefix[8], *body;
    uint32_t i;
    int ret;

    if ((c == NULL && count) || count > SG_CANDIDATES_MAX_LABELS) {
        return -EINVAL;
    }
    body = malloc((size_t)count * CAND_SIZE + 1);
    if (body == NULL) {
        return -ENOMEM;
    }
    /* 逐字段写成小端，不直接发结构体内存（不依赖编译器的排布） */
    for (i = 0; i < count; ++i) {
        unsigned char *p = body + (size_t)i * CAND_SIZE;
        uint32_t fx, fy;

        memcpy(&fx, &c[i].cx, 4);
        memcpy(&fy, &c[i].cy, 4);
        put32(p,      c[i].x0);
        put32(p + 4,  c[i].y0);
        put32(p + 8,  c[i].x1);
        put32(p + 12, c[i].y1);
        put32(p + 16, c[i].area);
        put32(p + 20, c[i].peak);
        put32(p + 24, fx);
        put32(p + 28, fy);
    }
    put32(prefix, count);
    put32(prefix + 4, truncated ? 1u : 0u);
    ret = send_message(t, TYPE_CANDS, block_id, prefix, sizeof(prefix),
                       body, (size_t)count * CAND_SIZE, timeout_ms);
    free(body);
    return ret;
}

int tx_tcp_send_status(struct tx_tcp *t, uint32_t block_id, const char *json, uint32_t timeout_ms)
{
    if (json == NULL) {
        return -EINVAL;
    }
    return send_message(t, TYPE_STATUS, block_id, json, strlen(json), NULL, 0, timeout_ms);
}

/* ==== 小工具 =========================================================== */

int tx_tcp_parse_target(const char *text, char *ip, uint16_t *port)
{
    const char *colon;
    size_t n;

    if (text == NULL || ip == NULL || port == NULL) {
        return -EINVAL;
    }
    colon = strchr(text, ':');
    n = colon ? (size_t)(colon - text) : strlen(text);
    if (n == 0 || n > 15) {
        return -EINVAL;
    }
    memcpy(ip, text, n);
    ip[n] = '\0';
    *port = TX_TCP_DEFAULT_PORT;
    if (colon) {
        char *end;
        unsigned long v = strtoul(colon + 1, &end, 10);

        if (*end || v == 0 || v > 65535) {
            return -EINVAL;
        }
        *port = (uint16_t)v;
    }
    return 0;
}

/* ==== 流水线出口 ======================================================= */

static int tcp_publish(void *context, const struct sg_image_view *view,
                       const struct sg_processing_result *r, uint32_t timeout_ms)
{
    struct tx_tcp_sink *s = context;
    int ret;

    s->seen++;
    if (s->candidates != NULL) {
        const struct sg_candidate *c;
        uint32_t count;
        int truncated;

        ret = sg_candidates_last(s->candidates, &c, &count, &truncated);
        if (!ret) {
            ret = tx_tcp_send_candidates(s->tcp, view->card_block_id, c, count, truncated, timeout_ms);
        }
        if (ret) {
            return ret;
        }
    }
    /* 抽样发原图：只在行紧挨着存放（stride == width）时能直接发 */
    if (s->frame_every && s->seen % s->frame_every == 0 && view->stride == view->width) {
        ret = send_pixels(s->tcp, view->card_block_id, view->pixels, view->width, view->height,
                          timeout_ms);
        if (ret) {
            return ret;
        }
    }
    if (s->next.publish != NULL) {
        return s->next.publish(s->next.context, view, r, timeout_ms);
    }
    return 0;
}

void tx_tcp_sink(struct tx_tcp_sink *s, struct sg_result_sink *sink)
{
    sink->context = s;
    sink->publish = tcp_publish;
}
