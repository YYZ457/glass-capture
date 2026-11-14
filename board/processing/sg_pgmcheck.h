/* SPDX-License-Identifier: MIT
 *
 * sg_pgmcheck.h —— 在板上检查一张 Mono8 图（verify/V1_testpattern.py 的 C 版本，板上没有 Python）。
 *
 * 回答三个问题：
 *   1. 亮度：min / max / 均值 / 1%、50%、99% 分位 / 饱和像素数（调曝光、增益用）
 *   2. 位：8 个位各自有没有出现过 1；按列 %3（Base 3 Tap 的三路）分开的 OR（查最高两位丢没丢）
 *   3. 测试图：和几种渐变公式逐像素比，各有多少像素不一致；是不是全 0 / 全 255
 * 纯函数，不碰硬件，电脑上有单元测试。
 */
#ifndef SG_PGMCHECK_H
#define SG_PGMCHECK_H
#include <stddef.h>
#include <stdint.h>

enum {
    SG_RAMP_X,          /* x % 256 */
    SG_RAMP_X_START,    /* (x + 第一行第一个像素) % 256 */
    SG_RAMP_SCALED,     /* x * 256 / width */
    SG_RAMP_ROW_START,  /* (x + 本行第一个像素) % 256，移动渐变（TestPattern 5）用 */
    SG_RAMP_COUNT
};

struct sg_pgm_check {
    uint32_t width, height;
    uint8_t min, max, p1, p50, p99;
    double mean;
    uint64_t saturated;                 /* = 255 的像素数 */
    uint8_t or_all, or_tap[3];          /* 所有像素按位或；按 x % 3 分开 */
    uint64_t ramp_mismatch[SG_RAMP_COUNT];
    int all_zero, all_255;
};

/* img 是 width × height 字节（行优先）。参数不对返回 -EINVAL。 */
int sg_pgm_check(const uint8_t *img, uint32_t width, uint32_t height, struct sg_pgm_check *out);

/* 缩略图：每 factor × factor 块取平均。out 至少 (width/factor) × (height/factor) 字节。
 * 返回缩略图宽、高；factor 为 0 或图比 factor 还小返回 -EINVAL。 */
int sg_pgm_thumb(const uint8_t *img, uint32_t width, uint32_t height, uint32_t factor,
                 uint8_t *out, uint32_t *out_w, uint32_t *out_h);

/* 公式名（打印用）。 */
const char *sg_ramp_name(int which);

#endif
