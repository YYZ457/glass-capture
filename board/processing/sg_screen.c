/* SPDX-License-Identifier: MIT
 *
 * sg_screen.c —— 见 sg_screen.h。
 *
 * 结构：一行切成 64 像素一段。chunk_any() 回答"这一段里有没有命中"（NEON：4 次 16 字节比较）；
 * 没有就跳过，有才逐像素扫这一段、生成 run。run 可以跨段：用 open / x0 / bright 记着还没结束的那个。
 */
#include <stddef.h>
#include <string.h>

#include "sg_screen.h"
#include "sg_err.h"

#define CHUNK 64u

#if defined(__aarch64__)
#include <arm_neon.h>

const char *sg_screen_backend(void) { return "neon"; }

static inline int chunk_any(const uint8_t *p, const uint8_t *lo, const uint8_t *hi)
{
    uint8x16_t acc = vdupq_n_u8(0);
    unsigned i;

    for (i = 0; i < CHUNK; i += 16) {
        uint8x16_t px = vld1q_u8(p + i);
        uint8x16_t hit = vorrq_u8(vcltq_u8(px, vld1q_u8(lo + i)), vcgtq_u8(px, vld1q_u8(hi + i)));
        acc = vorrq_u8(acc, hit);
    }
    return vmaxvq_u8(acc) != 0;
}
#else
const char *sg_screen_backend(void) { return "generic"; }

static inline int chunk_any(const uint8_t *p, const uint8_t *lo, const uint8_t *hi)
{
    unsigned i, any = 0;

    for (i = 0; i < CHUNK; ++i) {
        any |= (unsigned)(p[i] < lo[i]) | (unsigned)(p[i] > hi[i]);
    }
    return any != 0;
}
#endif

struct emit {
    struct sg_run *runs;
    uint32_t capacity, count;
    struct sg_screen_stats *st;
};

static void close_run(struct emit *e, uint32_t y, uint32_t x0, uint32_t x1, int bright)
{
    if (bright) e->st->bright_pixels += x1 - x0;
    else        e->st->dark_pixels += x1 - x0;
    e->st->runs++;
    if (e->count < e->capacity) {
        struct sg_run *r = &e->runs[e->count++];
        r->y = y;
        r->x0 = (uint16_t)x0;
        r->x1 = (uint16_t)x1;
        r->bright = (uint8_t)bright;
    }
}

int sg_screen(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride,
              const uint8_t *lo, const uint8_t *hi,
              struct sg_run *runs, uint32_t capacity, uint32_t *count, struct sg_screen_stats *stats)
{
    struct emit e;
    uint32_t y;

    if (!pixels || !lo || !hi || !count || !stats || (!runs && capacity) ||
        width == 0 || width > SG_SCREEN_MAX_WIDTH || height == 0 || stride < width) {
        return -EINVAL;
    }
    memset(stats, 0, sizeof *stats);
    e.runs = runs;
    e.capacity = capacity;
    e.count = 0;
    e.st = stats;

    for (y = 0; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t x = 0, x0 = 0;
        int open = 0, bright = 0;       /* open：有一个还没结束的 run，从 x0 开始，极性 bright */

        while (x < width) {
            uint32_t end = x + CHUNK <= width ? x + CHUNK : width;

            if (end - x == CHUNK && !chunk_any(row + x, lo + x, hi + x)) {
                if (open) {             /* 这一段全不命中：上一段留下的 run 到此为止 */
                    close_run(&e, y, x0, x, bright);
                    open = 0;
                }
                x = end;
                continue;
            }
            for (; x < end; ++x) {
                int cls = row[x] < lo[x] ? 1 : row[x] > hi[x] ? 2 : 0;   /* 0 不命中，1 暗，2 亮 */

                if (open && cls != (bright ? 2 : 1)) {
                    close_run(&e, y, x0, x, bright);
                    open = 0;
                }
                if (cls && !open) {
                    open = 1;
                    x0 = x;
                    bright = cls == 2;
                }
            }
        }
        if (open) {
            close_run(&e, y, x0, width, bright);
        }
    }
    *count = e.count;
    return stats->runs > e.count ? SG_SCREEN_TRUNCATED : 0;
}
