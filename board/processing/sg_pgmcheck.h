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

/* ---- 噪声模型（光子转移）：静止场景的一张图就够 ----
 * 线扫相机对着不动的东西拍，每一列在时间上是同一个点 → 每列的"均值—方差"就是一个采样点。
 * 画面里有亮有暗（比如一条明暗边界）时，一张图就能覆盖很多个亮度，拟合
 *     方差 = read_var + slope × 均值
 * 得到读出噪声（√read_var，单位 DN）和每个 DN 对应多少电子（1 / slope）。
 * 方差用相邻两行之差算（Σ(p[y+1] − p[y])² ÷ 2(h−1)），不怕光源慢慢漂；
 * 同时给出普通方差 var_total：它比 var_diff 大很多 = 场景在动或光在闪，这张图不能用来算噪声。
 * 有饱和像素（255）的列不参与。结果用于 docs/design/detection_plan.md 第 6.2 节第 2 步。 */
#define SG_NOISE_BINS 18
struct sg_noise_bin {
    uint32_t lo, hi;                 /* 列均值落在 [lo, hi) 的列归这一档 */
    uint32_t columns;
    double mean, var_diff, var_total;
};
struct sg_pgm_noise {
    struct sg_noise_bin bin[SG_NOISE_BINS];
    uint32_t saturated_columns;
    double tap_mean[3], tap_var[3];  /* 按列 % 3（Base 3 Tap 的三路）分开：三路差得多 = 通道增益不一致 */
    int fit_ok;                      /* 至少 3 档、每档 >= 16 列、斜率为正才拟合 */
    uint32_t fit_bins;
    double read_var, slope;          /* 方差 = read_var + slope × 均值 */
};

/* 需要 height >= 2。内存不够返回 -ENOMEM。 */
int sg_pgm_noise(const uint8_t *img, uint32_t width, uint32_t height, struct sg_pgm_noise *out);

/* ---- 线轮廓和对焦指标（装调、对焦用）----
 * 线扫相机对着不动的靶标拍，把所有行平均成一条线 m[x]（噪声被平均掉），然后：
 *   轮廓：把宽度分成 bins 段，每段给出 m 的均值 / 最小 / 最大（×10 取整，0..2550），画出来就是"示波器"视图；
 *   锐度 sharpness = 1000 × RMS(梯度) ÷ (对比度 + 1)，梯度 = (m[x+1] − m[x−1]) / 2，
 *        对比度 = m 的 99% 分位 − 1% 分位。拧对焦环时它越大越清楚；除以对比度是为了不受曝光影响；
 *   分段锐度：宽度方向 8 段各自的锐度。两头和中间差不多 = 相机和玻璃平行；一头高一头低 = 相机歪了；
 *   最强边缘：梯度最大处的 10%–90% 上升宽度（像素）。靶标是一条黑白直边时，这就是"焦点有多实"，
 *        目标 ≤ 2 像素（检测方案第 6.2 节）。两侧平台差不到 8 个灰度级时不报（edge_found = 0）。 */
#define SG_PROFILE_MAX_BINS 1024
#define SG_FOCUS_SEGMENTS 8
struct sg_pgm_profile {
    uint32_t bins;
    uint16_t mean[SG_PROFILE_MAX_BINS], min[SG_PROFILE_MAX_BINS], max[SG_PROFILE_MAX_BINS];
    double sharpness, contrast, seg_sharpness[SG_FOCUS_SEGMENTS];
    int edge_found;
    uint32_t edge_x;
    double edge_width_px, edge_contrast;
};

/* bins 在 8..SG_PROFILE_MAX_BINS 且不超过 width；width >= 128。参数不对 -EINVAL，内存不够 -ENOMEM。 */
int sg_pgm_profile(const uint8_t *img, uint32_t width, uint32_t height, uint32_t bins, struct sg_pgm_profile *out);

/* 公式名（打印用）。 */
const char *sg_ramp_name(int which);

#endif
