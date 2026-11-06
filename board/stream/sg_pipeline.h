/* SPDX-License-Identifier: MIT
 *
 * sg_pipeline.h —— 连续采集流水线：采集线程 → 有界队列 → 处理线程 → 结果出口。
 * 三个"插口"都是回调表：sg_source（数据从哪来）、sg_processor（怎么处理）、
 * sg_result_sink（结果往哪去）。所以它不知道硬件、算法、网络的任何细节。
 *
 * 状态：🟡 定义原样来自 legacy sg_pipeline.h；合成数据测过，从没在实机上跑过
 */
#ifndef SG_PIPELINE_H
#define SG_PIPELINE_H
#include <signal.h>
#include "sg_processing.h"
struct sg_source {
    void *context;
    /* Ownership of a successful frame moves to the caller. On failure no data. */
    int (*next)(void *, struct sg_capture_frame *);
    int (*stop)(void *); /* must be bounded; called exactly once by run */
};
struct sg_pipeline_options {
    unsigned queue_depth, processing_timeout_ms;
    uint64_t max_blocks, duration_ms, max_frame_bytes;
    int synthetic, strict_block_ids;
    volatile sig_atomic_t *cancel;
};
struct sg_pipeline_stats {
    uint64_t received, processed, bytes, received_lines, id_discontinuities;
    uint64_t queue_full_events, started_ns, finished_ns;
    unsigned queue_peak;
    int source_error, processing_error, output_error, stop_error, cancelled;
};
struct sg_result_sink {
    void *context;
    /* Borrowed data; must return within timeout_ms. Negative errno stops session.
     * Every completed processing result calls publish exactly once. UI transport
     * may explicitly decimate measurements; industrial decisions must not be silently dropped.
     */
    int (*publish)(void *, const struct sg_image_view *, const struct sg_processing_result *,
                   uint32_t timeout_ms);
};
uint64_t sg_monotonic_ns(void);
int sg_pipeline_run(struct sg_source *, struct sg_processor *, struct sg_result_sink *,
                    const struct sg_pipeline_options *, struct sg_pipeline_stats *);
#endif
