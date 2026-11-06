/* SPDX-License-Identifier: MIT
 *
 * sg_stats.c —— 最简单的 CPU 处理器：逐像素统计最小/最大/总和/超阈值个数。
 * 职责：诊断用（看图有没有信号、是不是全黑），**不是**缺陷判定，decision 恒为 SG_UNKNOWN。
 * 依赖：sg_processing.h
 * 迁移来源：legacy sg_processing.c 第 17-54 行（ms、stats、stats_close、sg_stats_open）
 * 怎么验证：喂一张已知图（如全 0 + 一个 255 的点），sum / maximum / above_threshold 应可手算。
 * 状态：🟡 合成数据测过
 */
#include "sg_processing.h"
#include "sg_err.h"

int sg_stats_open(struct sg_processor *p, uint32_t threshold)
{ (void)p; (void)threshold; return -ENOSYS; /* TODO */ }
