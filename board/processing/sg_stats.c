/* SPDX-License-Identifier: MIT
 *
 * sg_stats.c —— 最简单的 CPU 处理器：逐像素统计最小/最大/总和/超阈值个数。
 * 诊断用（看图有没有信号、是不是全黑），不是缺陷判定，decision 恒为 SG_UNKNOWN。
 * 迁移来源：legacy sg_processing.c 第 17-54 行
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sg_processing.h"
#include "sg_err.h"

static uint64_t now_ms(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* sg_processor.process 的实现。context 里存的是阈值。 */
static int stats_process(void *context, const struct sg_image_view *v, uint32_t timeout_ms,
                         struct sg_processing_result *r)
{
    uint32_t threshold = *(uint32_t *)context;
    uint64_t deadline = now_ms() + timeout_ms;
    uint32_t x, y;
    int ret;

    ret = sg_view_validate(v);
    if (ret) {
        return ret;
    }
    if (r == NULL || timeout_ms == 0 || timeout_ms > 60000) {
        return -EINVAL;
    }

    memset(r, 0, sizeof(*r));
    r->abi         = SG_PROCESS_ABI;
    r->algorithm   = 1;
    r->sequence    = v->sequence;
    r->first_line  = v->first_line;
    r->pixel_count = (uint64_t)v->width * v->height;
    r->decision    = SG_UNKNOWN;
    r->minimum     = 255;

    for (y = 0; y < v->height; ++y) {
        const unsigned char *row = v->pixels + (size_t)y * v->stride;

        if (now_ms() >= deadline) {     /* 每行查一次超时 */
            return -ETIMEDOUT;
        }
        for (x = 0; x < v->width; ++x) {
            unsigned value = row[x];

            r->sum += value;
            if (value > threshold) {
                r->above_threshold++;
            }
            if (value < r->minimum) {
                r->minimum = value;
            }
            if (value > r->maximum) {
                r->maximum = value;
            }
        }
    }
    return 0;
}

static void stats_close(void *context)
{
    free(context);
}

int sg_stats_open(struct sg_processor *p, uint32_t threshold)
{
    uint32_t *setting;

    if (p == NULL || threshold > 255) {
        return -EINVAL;
    }
    setting = malloc(sizeof(*setting));
    if (setting == NULL) {
        return -ENOMEM;
    }
    *setting   = threshold;
    p->context = setting;
    p->process = stats_process;
    p->close   = stats_close;
    return 0;
}
