/* SPDX-License-Identifier: MIT
 *
 * sg_bench.c —— sg3000-capture bench [--repeat 3]
 *
 * 回答"四个 A53 核处理 246 MB/s 够不够"（docs/design/detection_plan.md 第 4 节）：
 * 造一块和实采一样大的图（16384 × 2000，背景 170 加一点噪声），分别量
 *   memcpy            内存带宽的参照（DMA 拷出来就是这个速度量级）
 *   stats             sg3000-stream --processor stats 用的处理器
 *   candidates        现在的 sg_candidates_find（逐像素浮点 + 连通域）
 *   screen            快筛原型 sg_screen（干净图）
 *   screen_2000       快筛，图上有 2000 个 6 × 6 的暗块（很脏的玻璃）
 *   candidates_2000   现在的 sg_candidates_find，同一张脏图
 * 每项跑 repeat 次取最快的一次，打印 MB/s 和它是 15 kHz 数据率（245.76 MB/s）的多少倍。
 * x15k < 1 = 这个环节一个核跟不上。
 *
 * 不打开设备、不碰硬件，随时可以跑。想看几个核同时跑时内存带宽够不够：同时起 3 个进程（board_session.sh 第 8 步）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sg_bench.h"
#include "sg_candidates.h"
#include "sg_err.h"
#include "sg_screen.h"

enum { BENCH_W = 16384, BENCH_H = 2000, BENCH_BG = 170, BENCH_MARGIN = 12, BENCH_PARTICLES = 2000 };
#define BENCH_TARGET_MBPS 245.76      /* 15000 行 / 秒 × 16384 字节 */
#define BENCH_MAX_RUNS 200000u
#define BENCH_MAX_CANDS 20000u

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static uint32_t rng_state = 2463534242u;
static uint32_t rng(void)       /* xorshift32：只要快、可重复 */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void report(const char *name, double best_ms, size_t bytes, const char *extra)
{
    double mbps = (double)bytes / 1e6 / (best_ms / 1e3);

    printf("Bench name=%s ms=%.1f MBps=%.0f x15k=%.2f%s%s\n", name, best_ms, mbps,
           mbps / BENCH_TARGET_MBPS, extra[0] ? " " : "", extra);
}

int sg_bench_main(int argc, char **argv)
{
    const size_t n = (size_t)BENCH_W * BENCH_H;
    unsigned long repeat = 3;
    uint8_t *clean, *dirty, *copy, *lo, *hi;
    float *mean;
    struct sg_candidate *cands;
    struct sg_run *runs;
    struct sg_image_view view;
    struct sg_background bg;
    struct sg_candidate_params params = { BENCH_MARGIN, 2, 0, BENCH_MAX_CANDS };
    struct sg_processor stats;
    struct sg_processing_result result;
    struct sg_screen_stats st;
    uint32_t count = 0, i;
    double best, t;
    char extra[96];
    unsigned long r;
    size_t k;
    int a, ret = 0;

    for (a = 2; a < argc; ++a) {
        char *end;

        if (!strcmp(argv[a], "--repeat") && a + 1 < argc) {
            repeat = strtoul(argv[++a], &end, 10);
            if (*end || repeat < 1 || repeat > 100) return 2;
        } else {
            return 2;
        }
    }
    clean = malloc(n);
    dirty = malloc(n);
    copy = malloc(n);
    lo = malloc(BENCH_W);
    hi = malloc(BENCH_W);
    mean = malloc(BENCH_W * sizeof *mean);
    cands = malloc(BENCH_MAX_CANDS * sizeof *cands);
    runs = malloc(BENCH_MAX_RUNS * sizeof *runs);
    if (!clean || !dirty || !copy || !lo || !hi || !mean || !cands || !runs || sg_stats_open(&stats, 128)) {
        fprintf(stderr, "Failed: out of memory (bench needs about 100 MB)\n");
        return 1;
    }
    /* 背景 170，噪声是四个 0..3 的均匀数之和减 6（标准差约 2.2，范围 ±6），永远碰不到 ±12 的阈值 */
    for (k = 0; k < n; ++k) {
        uint32_t q = rng();
        clean[k] = (uint8_t)(BENCH_BG - 6 + (q & 3) + ((q >> 2) & 3) + ((q >> 4) & 3) + ((q >> 6) & 3));
    }
    memcpy(dirty, clean, n);
    for (i = 0; i < BENCH_PARTICLES; ++i) {         /* 6 × 6 像素的暗块 ≈ 30 µm 的不透光颗粒 */
        uint32_t x = rng() % (BENCH_W - 8), y = rng() % (BENCH_H - 8), dy;
        for (dy = 0; dy < 6; ++dy) memset(dirty + (size_t)(y + dy) * BENCH_W + x, 20, 6);
    }
    for (i = 0; i < BENCH_W; ++i) {
        lo[i] = BENCH_BG - BENCH_MARGIN;
        hi[i] = BENCH_BG + BENCH_MARGIN;
        mean[i] = (float)BENCH_BG;
    }
    memset(&view, 0, sizeof view);
    view.pixels = clean;
    view.width = BENCH_W;
    view.height = BENCH_H;
    view.stride = BENCH_W;
    view.bytes = n;
    view.pixel_format = SG_PIXEL_MONO8;
    bg.width = BENCH_W;
    bg.column_mean = mean;
    bg.column_sigma = NULL;

    printf("BenchInfo width=%d height=%d bytes=%zu repeat=%lu screen=%s target_MBps=%.2f\n",
           BENCH_W, BENCH_H, n, repeat, sg_screen_backend(), BENCH_TARGET_MBPS);

    for (best = 1e30, r = 0; r < repeat; ++r) {
        t = now_ms(); memcpy(copy, clean, n); t = now_ms() - t;
        if (t < best) best = t;
    }
    report("memcpy", best, n, copy[n / 2] == clean[n / 2] ? "" : "MISMATCH");

    for (best = 1e30, r = 0; r < repeat && !ret; ++r) {
        t = now_ms(); ret = stats.process(stats.context, &view, 60000, &result); t = now_ms() - t;
        if (t < best) best = t;
    }
    if (ret) goto fail;
    snprintf(extra, sizeof extra, "min=%u max=%u", result.minimum, result.maximum);
    report("stats", best, n, extra);

    for (best = 1e30, r = 0; r < repeat && ret >= 0; ++r) {
        t = now_ms(); ret = sg_candidates_find(&view, &bg, &params, cands, BENCH_MAX_CANDS, &count); t = now_ms() - t;
        if (t < best) best = t;
    }
    if (ret < 0) goto fail;
    snprintf(extra, sizeof extra, "candidates=%u", count);
    report("candidates", best, n, extra);

    for (best = 1e30, r = 0; r < repeat && ret >= 0; ++r) {
        t = now_ms(); ret = sg_screen(clean, BENCH_W, BENCH_H, BENCH_W, lo, hi, runs, BENCH_MAX_RUNS, &count, &st); t = now_ms() - t;
        if (t < best) best = t;
    }
    if (ret < 0) goto fail;
    snprintf(extra, sizeof extra, "runs=%llu", (unsigned long long)st.runs);
    report("screen", best, n, extra);

    for (best = 1e30, r = 0; r < repeat && ret >= 0; ++r) {
        t = now_ms(); ret = sg_screen(dirty, BENCH_W, BENCH_H, BENCH_W, lo, hi, runs, BENCH_MAX_RUNS, &count, &st); t = now_ms() - t;
        if (t < best) best = t;
    }
    if (ret < 0) goto fail;
    snprintf(extra, sizeof extra, "runs=%llu hit_pixels=%llu", (unsigned long long)st.runs,
             (unsigned long long)(st.dark_pixels + st.bright_pixels));
    report("screen_2000", best, n, extra);

    view.pixels = dirty;
    for (best = 1e30, r = 0; r < repeat && ret >= 0; ++r) {
        t = now_ms(); ret = sg_candidates_find(&view, &bg, &params, cands, BENCH_MAX_CANDS, &count); t = now_ms() - t;
        if (t < best) best = t;
    }
    if (ret < 0) goto fail;
    snprintf(extra, sizeof extra, "candidates=%u", count);
    report("candidates_2000", best, n, extra);
    ret = 0;

fail:
    if (ret < 0) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    stats.close(stats.context);
    free(clean); free(dirty); free(copy); free(lo); free(hi); free(mean); free(cands); free(runs);
    return ret ? 1 : 0;
}
