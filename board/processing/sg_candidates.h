/* SPDX-License-Identifier: MIT
 *
 * sg_candidates.h —— 颗粒候选检测：背景校正 → 阈值 → 连通域 → 候选列表。⬜ 全新
 *
 * 为什么要在板上做：满速原始数据约 252 MB/s，千兆网约 110 MB/s，原图出不了板子，
 * 只能发候选（见 docs/decisions/D002_bandwidth.md）。
 * 开发顺序：先在 host/ 用 Python 跑通、对上真值（verify/V4），再按同样算法写进这里，
 *           两边对同一张图输出必须一致。
 * 输出格式对齐 docs/interfaces/schemas/frame-candidates-v1.schema.json。
 */
#ifndef SG_CANDIDATES_H
#define SG_CANDIDATES_H
#include <stdint.h>
#include "sg_processing.h"

struct sg_candidate {
    uint32_t x0, y0, x1, y1;   /* 外接框，块内像素坐标 */
    uint32_t area;             /* 像素数 */
    uint32_t peak;             /* 校正后峰值 */
    float cx, cy;              /* 亚像素质心 */
};

struct sg_candidate_params {
    uint32_t threshold;        /* 校正后灰度阈值 */
    uint32_t min_area, max_area;
    uint32_t max_candidates;   /* 超过就截断并置标志，不能静默丢弃 */
};

/* 背景模型：逐列均值和噪声，由暗场/空玻璃图标定。 */
struct sg_background {
    uint32_t width;
    const float *column_mean;
    const float *column_sigma;
};

/* 处理一块，写入 out[0..*count)。返回 0 或负 errno。 */
int sg_candidates_find(const struct sg_image_view *v, const struct sg_background *bg,
                       const struct sg_candidate_params *p,
                       struct sg_candidate *out, uint32_t capacity, uint32_t *count);

#endif
