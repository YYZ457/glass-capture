/* SPDX-License-Identifier: MIT
 *
 * tx_jsonl.c —— 结果写成 "SGJSON {...}" 行到 stdout。
 * 迁移来源：legacy sg_stream_main.c 第 63-101 行（emit + publish）。
 * 输出文字和 legacy 一字不差：host/console/telemetry.py 按字段解析。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tx_jsonl.h"
#include "sg_err.h"

static uint64_t now_ms(void)
{
    return sg_monotonic_ns() / 1000000u;
}

int tx_jsonl_emit(const char *line, uint32_t timeout_ms)
{
    size_t length = strlen(line), offset = 0;
    uint64_t deadline = now_ms() + timeout_ms;

    while (offset < length) {
        struct pollfd fd = { STDOUT_FILENO, POLLOUT, 0 };
        uint64_t now = now_ms();
        ssize_t n;
        int ready;

        if (now >= deadline) {
            return -ETIMEDOUT;
        }
        /* 等 stdout 可写（串口慢的时候会满） */
        ready = poll(&fd, 1, (int)(deadline - now));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (ready == 0) {
            return -ETIMEDOUT;
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -EPIPE;
        }

        n = write(STDOUT_FILENO, line + offset, length - offset);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -errno;
        }
        if (n == 0) {
            return -EIO;
        }
        offset += (size_t)n;
    }
    return 0;
}

static const char *decision_name(uint32_t d)
{
    return d == SG_PASS ? "pass" : d == SG_FAIL ? "fail" : "unknown";
}

/* sg_result_sink.publish 的实现 */
static int jsonl_publish(void *context, const struct sg_image_view *view,
                         const struct sg_processing_result *r, uint32_t timeout_ms)
{
    struct tx_jsonl *t = context;
    uint64_t now = sg_monotonic_ns();
    char line[1800];
    int n;

    t->seen++;

    /* 抽样：UNKNOWN 的结果在 interval_ns 内只输出一条；PASS/FAIL 一条都不能少 */
    if (t->last_ns && now - t->last_ns < t->interval_ns && r->decision == SG_UNKNOWN) {
        return 0;
    }
    t->last_ns = now;

    n = snprintf(line, sizeof(line),
        "SGJSON {\"schema\":1,\"event\":\"result\",\"source\":\"%s\",\"processor\":\"%s\","
        "\"sequence\":\"%" PRIu64 "\",\"received_line_start\":\"%" PRIu64 "\","
        "\"received_ns\":\"%" PRIu64 "\","
        "\"block_id\":%u,\"width\":%u,\"height\":%u,\"stride\":%u,\"flags\":%u,"
        "\"processed_total\":\"%" PRIu64 "\",\"decision\":\"%s\",\"algorithm\":%u,"
        "\"pixel_count\":\"%" PRIu64 "\",\"sum\":\"%" PRIu64 "\","
        "\"above_threshold\":\"%" PRIu64 "\","
        "\"min\":%u,\"max\":%u,\"continuity_verified\":false}\n",
        t->source, t->processor,
        view->sequence, view->first_line, view->timestamp_ns,
        view->card_block_id, view->width, view->height, view->stride, view->flags,
        t->seen, decision_name(r->decision), r->algorithm,
        r->pixel_count, r->sum, r->above_threshold,
        r->minimum, r->maximum);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return -EOVERFLOW;
    }
    return tx_jsonl_emit(line, timeout_ms);
}

void tx_jsonl_sink(struct tx_jsonl *t, struct sg_result_sink *sink)
{
    sink->context = t;
    sink->publish = jsonl_publish;
}
