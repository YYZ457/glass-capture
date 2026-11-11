/* SPDX-License-Identifier: MIT
 *
 * unit_test.c —— processing / stream 的单元测试（纯计算，不碰硬件）。运行：make -C board test
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sg_processing.h"
#include "sg_candidates.h"
#include "sg_linecheck.h"
#include "sg_pipeline.h"

static struct sg_image_view view(unsigned char *p, uint32_t w, uint32_t h)
{
    struct sg_image_view v = {0};
    v.pixels = p; v.width = w; v.height = h; v.stride = w; v.bytes = (uint64_t)w * h;
    v.pixel_format = SG_PIXEL_MONO8; v.sequence = 7; v.first_line = 100;
    return v;
}

static void test_view_stats(void)
{
    unsigned char img[16] = {0};
    struct sg_image_view v = view(img, 4, 4);
    struct sg_processor p; struct sg_processing_result r;
    img[5] = 255; img[6] = 10;
    assert(sg_view_validate(&v) == 0);
    v.stride = 3; assert(sg_view_validate(&v) == -EINVAL); v.stride = 4;
    v.bytes = 15; assert(sg_view_validate(&v) == -EINVAL); v.bytes = 16;
    assert(sg_stats_open(&p, 256) == -EINVAL);
    assert(sg_stats_open(&p, 128) == 0);
    assert(p.process(p.context, &v, 1000, &r) == 0);
    assert(r.sum == 265 && r.maximum == 255 && r.minimum == 0 && r.above_threshold == 1);
    assert(r.decision == SG_UNKNOWN && r.sequence == 7 && r.first_line == 100 && r.pixel_count == 16);
    p.close(p.context);
    puts("view/stats ok");
}

static void test_candidates(void)
{
    enum { W = 64, H = 32 };
    static unsigned char img[W * H];
    float mean[W]; struct sg_background bg = { W, mean, NULL };
    struct sg_candidate c[8]; uint32_t n;
    struct sg_candidate_params p = { 20, 2, 0, 0 };
    struct sg_image_view v = view(img, W, H);
    int i;
    memset(img, 50, sizeof img);
    for (i = 0; i < W; ++i) mean[i] = 50;
    /* 亮团：3x3 at (10..12, 5..7)，中心最亮 */
    for (int y = 5; y <= 7; ++y) for (int x = 10; x <= 12; ++x) img[y * W + x] = 100;
    img[6 * W + 11] = 200;
    /* 暗团：对角相连两个像素 (40,20)(41,21) -> 8 邻接应合并 */
    img[20 * W + 40] = 0; img[21 * W + 41] = 0;
    /* 孤立单像素：面积 1 < min_area，丢掉 */
    img[30 * W + 60] = 255;
    /* U 形：两臂从上方分开，底部相连 -> 一个连通域（测试合并） */
    for (int y = 10; y <= 14; ++y) { img[y * W + 30] = 150; img[y * W + 34] = 150; }
    for (int x = 30; x <= 34; ++x) img[14 * W + x] = 150;

    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == 0);
    assert(n == 3);
    assert(c[0].x0 == 10 && c[0].y0 == 5 && c[0].x1 == 13 && c[0].y1 == 8 && c[0].area == 9 && c[0].peak == 150);
    assert(fabsf(c[0].cx - 11) < 1e-4 && fabsf(c[0].cy - 6) < 1e-4);
    assert(c[1].x0 == 30 && c[1].y0 == 10 && c[1].x1 == 35 && c[1].y1 == 15 && c[1].area == 13);
    assert(c[2].x0 == 40 && c[2].y1 == 22 && c[2].area == 2 && c[2].peak == 50);
    /* 截断 */
    p.max_candidates = 2;
    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == SG_CANDIDATES_TRUNCATED && n == 2);
    /* 面积上限 */
    p.max_candidates = 0; p.max_area = 9;
    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == 0 && n == 2);
    /* 无背景：用自身列均值，仍能找到亮团 */
    p.max_area = 0;
    assert(sg_candidates_find(&v, NULL, &p, c, 8, &n) >= 0 && n >= 1);
    /* 参数错误 */
    p.threshold = 0; assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == -EINVAL);
    puts("candidates ok");
}

static void test_linecheck(void)
{
    struct sg_linecheck c; struct sg_capture_frame f = {0};
    uint32_t ids[] = {1, 2, 3, 5, 6}; int jumps = 0;
    sg_linecheck_reset(&c);
    f.height = 100;
    for (int i = 0; i < 5; ++i) { f.block_id = ids[i]; jumps += sg_linecheck_feed(&c, &f, (uint64_t)i * 1000000000u); }
    assert(jumps == 1 && c.id_jumps == 1 && c.lines == 500 && c.blocks == 5);
    assert(fabs(sg_linecheck_line_rate(&c) - 100.0) < 1e-9);
    puts("linecheck ok");
}

/* 处理器返回错误 -> 流水线以 processing_error 结束，stop 只调一次 */
static int stops;
static uint32_t nid;
static int paced;
static int src_next(void *c, struct sg_capture_frame *f)
{ (void)c; if (paced) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); } memset(f, 0, sizeof *f); f->data = calloc(1, 64); f->width = 8; f->height = 8; f->length = 64; f->block_id = ++nid; return 0; }
static int src_stop(void *c) { (void)c; stops++; return 0; }
static int bad_process(void *c, const struct sg_image_view *v, uint32_t t, struct sg_processing_result *r)
{ (void)c; (void)v; (void)t; (void)r; return -EIO; }
static int pubs;
static int publish(void *c, const struct sg_image_view *v, const struct sg_processing_result *r, uint32_t t)
{ (void)c; (void)v; (void)r; (void)t; pubs++; return 0; }

static void test_pipeline(void)
{
    struct sg_source s = { NULL, src_next, src_stop };
    struct sg_processor p; struct sg_result_sink k = { NULL, publish };
    struct sg_pipeline_options o = { .queue_depth = 4, .processing_timeout_ms = 1000,
        .max_blocks = 1000, .max_frame_bytes = 4096 };
    struct sg_pipeline_stats st;
    assert(sg_stats_open(&p, 10) == 0);
    paced = 1; o.max_blocks = 300;
    assert(sg_pipeline_run(&s, &p, &k, &o, &st) == 0);
    assert(st.received == 300 && st.processed == 300 && pubs == 300 && st.id_discontinuities == 0);
    paced = 0; o.max_blocks = 1000;
    printf("pipeline normal: ret received=%llu processed=%llu peak=%u qfull=%llu stops=%d\n",
           (unsigned long long)st.received, (unsigned long long)st.processed, st.queue_peak,
           (unsigned long long)st.queue_full_events, stops);
    assert(stops == 1);
    p.close(p.context);
    p.process = bad_process; p.close = NULL; stops = 0;
    int ret = sg_pipeline_run(&s, &p, &k, &o, &st);
    printf("pipeline bad: ret=%d proc_err=%d src_err=%d stops=%d\n", ret, st.processing_error, st.source_error, stops);
    assert(st.processing_error == -EIO || st.source_error == -ENOBUFS);
    assert(ret == -EIO || ret == -ENOBUFS);
    assert(stops == 1);
    puts("pipeline ok");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_view_stats(); test_candidates(); test_linecheck(); test_pipeline();
    puts("unit: ALL PASS");
    return 0;
}
