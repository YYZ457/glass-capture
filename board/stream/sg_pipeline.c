/* SPDX-License-Identifier: MIT
 *
 * sg_pipeline.c —— 双线程流水线：主线程从 sg_source 取块放进有界队列，
 * 处理线程取出调用 sg_processor，再把结果交给 sg_result_sink。
 * 职责：只管排队、线程、计数、停止，不知道数据从哪来、怎么处理。
 * 依赖：sg_pipeline.h、pthread
 * 迁移来源：legacy sg_pipeline.c（137 行，整体搬入即可，无需拆分）
 *   struct slot / struct queue ← 第 9-26 行
 *   consume()（处理线程）     ← 第 27-57 行
 *   sg_pipeline_run           ← 第 58 行起
 * 注意：队列满时计数 queue_full_events，不静默丢块。
 * 怎么验证：
 *   1. 合成源（apps/sg_stream_main --synthetic）跑 10 万块，received = processed
 *   2. 实机：先确认 V2 行完整性通过，再看 queue_peak 是否逼近 queue_depth
 * 状态：🟡
 */
#include "sg_pipeline.h"
#include "sg_err.h"

uint64_t sg_monotonic_ns(void) { return 0; /* TODO */ }

int sg_pipeline_run(struct sg_source *src, struct sg_processor *proc, struct sg_result_sink *sink,
                    const struct sg_pipeline_options *o, struct sg_pipeline_stats *st)
{ (void)src; (void)proc; (void)sink; (void)o; (void)st; return -ENOSYS; /* TODO */ }
