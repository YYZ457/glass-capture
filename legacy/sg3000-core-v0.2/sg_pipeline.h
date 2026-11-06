/* SPDX-License-Identifier: MIT */
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
