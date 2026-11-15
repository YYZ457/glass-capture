/* SPDX-License-Identifier: MIT
 *
 * sg_stats.c —— 最简单的 CPU 处理器：逐像素统计最小/最大/总和/超阈值个数。
 * 诊断用（看图有没有信号、是不是全黑），不是缺陷判定，decision 恒为 SG_UNKNOWN。
 * 迁移来源：legacy sg_processing.c 第 17-54 行
 *
 * 2026-11-15 提速：原来的内层循环每个像素都读写结果结构体（ARM64 上约 13 条指令 / 像素），
 * 一个 A53 核估计只有 90–150 MB/s，跟不上 15 kHz 的 246 MB/s（方案第 4.2 节）。
 * 现在一行交给 sg_stats_row()：ARM64 用 NEON 一次 16 个像素，其他平台用只动局部变量的普通循环。
 * 两种实现结果必须完全相同（单元测试拿逐像素的笨办法对）。
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

#if defined(__aarch64__)
#include <arm_neon.h>
#define STATS_BLOCK 128u    /* 每 128 个向量（2048 像素）把 16 位累加器倒出来一次：128 × 2 × 255 = 65280，不溢出 */

static uint32_t stats_row_simd(const unsigned char *row, uint32_t n, uint32_t threshold,
                               uint64_t *sum, uint64_t *above, uint32_t *min, uint32_t *max)
{
    const uint8x16_t thr = vdupq_n_u8((uint8_t)threshold);
    uint8x16_t vmin = vdupq_n_u8(255), vmax = vdupq_n_u8(0);
    uint32_t x = 0, m;

    while (n - x >= 16) {
        uint32_t vectors = (n - x) / 16, i;
        uint16x8_t sum16 = vdupq_n_u16(0);
        uint8x16_t count = vdupq_n_u8(0);

        if (vectors > STATS_BLOCK) vectors = STATS_BLOCK;
        for (i = 0; i < vectors; ++i, x += 16) {
            uint8x16_t px = vld1q_u8(row + x);

            sum16 = vpadalq_u8(sum16, px);                    /* 相邻两个字节相加，累加到 16 位 */
            count = vsubq_u8(count, vcgtq_u8(px, thr));       /* 大于阈值的那几路是 0xFF，减它 = 加 1 */
            vmin = vminq_u8(vmin, px);
            vmax = vmaxq_u8(vmax, px);
        }
        *sum += vaddlvq_u16(sum16);
        *above += vaddlvq_u8(count);
    }
    m = vminvq_u8(vmin);
    if (m < *min) *min = m;
    m = vmaxvq_u8(vmax);
    if (m > *max) *max = m;
    return x;
}
#else
static uint32_t stats_row_simd(const unsigned char *row, uint32_t n, uint32_t threshold,
                               uint64_t *sum, uint64_t *above, uint32_t *min, uint32_t *max)
{
    (void)row; (void)n; (void)threshold; (void)sum; (void)above; (void)min; (void)max;
    return 0;
}
#endif

void sg_stats_row(const unsigned char *row, uint32_t n, uint32_t threshold,
                  uint64_t *sum, uint64_t *above, uint32_t *min, uint32_t *max)
{
    uint32_t x = stats_row_simd(row, n, threshold, sum, above, min, max);
    uint32_t s = 0, a = 0, lo = *min, hi = *max;   /* 一行最多 65535 × 255，32 位够 */

    for (; x < n; ++x) {
        uint32_t value = row[x];

        s += value;
        a += value > threshold;
        if (value < lo) lo = value;
        if (value > hi) hi = value;
    }
    *sum += s;
    *above += a;
    *min = lo;
    *max = hi;
}

/* sg_processor.process 的实现。context 里存的是阈值。 */
static int stats_process(void *context, const struct sg_image_view *v, uint32_t timeout_ms,
                         struct sg_processing_result *r)
{
    uint32_t threshold = *(uint32_t *)context;
    uint64_t deadline = now_ms() + timeout_ms;
    uint32_t x, y, n;
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
        for (x = 0; x < v->width; x += n) {      /* 一次最多 65535 个像素，保证 sg_stats_row 里的 32 位和不溢出 */
            n = v->width - x > 65535u ? 65535u : v->width - x;
            sg_stats_row(row + x, n, threshold, &r->sum, &r->above_threshold, &r->minimum, &r->maximum);
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
