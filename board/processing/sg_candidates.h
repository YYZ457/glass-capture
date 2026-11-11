/* SPDX-License-Identifier: MIT
 *
 * sg_candidates.h —— 颗粒候选检测：背景校正 → 阈值 → 连通域 → 候选列表。
 *
 * 为什么要在板上做：满速原始数据约 252 MB/s，千兆网约 110 MB/s，原图出不了板子，
 * 只能发候选（见 docs/decisions/D002_bandwidth.md）。
 *
 * 算法（第一版，参数待 verify/V4 真值比对后再定）：
 *   1. 背景校正：signed = 像素 - 该列背景均值
 *   2. 阈值：|signed| >= threshold 的像素算"可疑"（亮点、暗点都找）
 *   3. 连通域：8 邻接，把相邻的可疑像素归成一组
 *   4. 每组算外接框、面积、峰值、加权质心；面积不在 [min_area, max_area] 的丢掉
 * 约定：外接框半开区间 [x0, x1) × [y0, y1)，和 frame-candidates-v1 一致；
 *       坐标是块内像素坐标，质心以像素中心为整数点。
 * 输出顺序：按每个候选第一个像素的光栅顺序（从上到下、从左到右），结果是确定的。
 */
#ifndef SG_CANDIDATES_H
#define SG_CANDIDATES_H
#include <stdint.h>
#include "sg_processing.h"

struct sg_candidate {
    uint32_t x0, y0, x1, y1;   /* 外接框，半开区间 */
    uint32_t area;             /* 像素数 */
    uint32_t peak;             /* 校正后 |signed| 的最大值，四舍五入 */
    float cx, cy;              /* 以 |signed| 加权的质心 */
};

struct sg_candidate_params {
    uint32_t threshold;        /* 1..255，校正后灰度阈值 */
    uint32_t min_area;         /* >= 1 */
    uint32_t max_area;         /* 0 = 不限 */
    uint32_t max_candidates;   /* 0 = 只受 capacity 限制；超过就截断并返回 SG_CANDIDATES_TRUNCATED */
};

/* 背景模型：逐列均值和噪声，由暗场/空玻璃图标定（verify/V3 输出）。
 * column_sigma 暂未使用，留给"按每列噪声定阈值"的下一版。 */
struct sg_background {
    uint32_t width;
    const float *column_mean;
    const float *column_sigma;
};

/* 返回值 1：结果被截断（候选比 max_candidates / capacity 多），out 里是前面那些。 */
#define SG_CANDIDATES_TRUNCATED 1

/* 一块里临时连通域标号的上限。超过说明阈值太低、图里全是"可疑"像素，直接返回 -E2BIG。 */
#define SG_CANDIDATES_MAX_LABELS (1u << 20)

/* 处理一块，写入 out[0..*count)。
 * bg 为 NULL 时，用这一块自己的逐列均值当背景（没有标定数据时的退路）。
 * 返回 0、SG_CANDIDATES_TRUNCATED 或负 errno。 */
int sg_candidates_find(const struct sg_image_view *v, const struct sg_background *bg,
                       const struct sg_candidate_params *p,
                       struct sg_candidate *out, uint32_t capacity, uint32_t *count);

#endif
