/* SPDX-License-Identifier: MIT */
#ifndef SG_PROCESSING_H
#define SG_PROCESSING_H
#include <stdint.h>
#include "sg3000_capture_core.h"
#define SG_PROCESS_ABI 1u
#define SG_PIXEL_MONO8 0x01080001u
#define SG_META_ID_DISCONTINUITY 1u
#define SG_META_SYNTHETIC 2u
#define SG_META_CONTINUITY_UNVERIFIED 4u
enum sg_decision { SG_UNKNOWN = 0, SG_PASS = 1, SG_FAIL = 2 };
/* Local C interface, not a network wire struct. Pixels are borrowed until
 * process() returns. No callback may retain or DMA to this pointer afterwards.
 * first_line counts RECEIVED rows, not physical encoder position. timestamp_ns
 * is host monotonic receive-completion time, not camera exposure time.
 */
struct sg_image_view {
    const unsigned char *pixels;
    uint64_t sequence, first_line, timestamp_ns, bytes;
    uint32_t width, height, stride, pixel_format, card_block_id, flags;
};
struct sg_processing_result {
    uint32_t abi, decision, error_flags, algorithm;
    uint64_t sequence, first_line, pixel_count, sum, above_threshold;
    uint32_t minimum, maximum;
};
struct sg_processor {
    void *context;
    /* Mandatory finite timeout, negative errno on failure. */
    int (*process)(void *, const struct sg_image_view *, uint32_t timeout_ms,
                   struct sg_processing_result *);
    void (*close)(void *);
};
int sg_stats_open(struct sg_processor *, uint32_t threshold);
int sg_pl_open(struct sg_processor *, const char *device);
int sg_view_validate(const struct sg_image_view *);
#endif
