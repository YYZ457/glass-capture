/* SPDX-License-Identifier: MIT */
#ifndef SG3000_CAPTURE_CORE_H
#define SG3000_CAPTURE_CORE_H
#include <stddef.h>
#include <stdint.h>
#define SG_CAPTURE_MAX_BYTES (256u * 1024u * 1024u)
#define SG_CAPTURE_DMA_CHUNK 65536u

/* Operations return zero or negative errno. DMA copies exactly length bytes
 * only on success. now_ms must be monotonic; sleep_ms must advance/wait time.
 * The callbacks own hardware ordering and DMA lifetime, including cancellation.
 */
struct sg_capture_io {
    void *context;
    int (*read_reg)(void *, uint32_t, uint32_t *);
    int (*write_reg)(void *, uint32_t, uint32_t);
    int (*dma_read)(void *, uint32_t, void *, uint32_t, uint32_t);
    uint64_t (*now_ms)(void *);
    void (*sleep_ms)(void *, uint32_t);
    int (*cancelled)(void *);
};
struct sg_capture_options {
    uint32_t control_timeout_ms, frame_timeout_ms, dma_timeout_ms;
    size_t max_frame_bytes;
};
struct sg_capture_session {
    struct sg_capture_io io;
    uint16_t request_id, last_ack;
    int have_ack;
};
struct sg_capture_preflight {
    uint32_t ddr_status, pixel_clocks;
    uint32_t cl_configuration, camera_color, camera_depth;
    uint32_t valid_mask; /* bits 0..4: DDR, clocks, CL, color, depth read successfully */
};
struct sg_capture_frame {
    unsigned char *data;
    size_t length;
    uint32_t width, height, block_id, packets;
};
struct sg_capture_result {
    struct sg_capture_preflight preflight;
    struct sg_capture_frame frame;
    int primary_error, stop_error;
    int start_attempted, stop_attempted;
};
/* Owns one acquisition START/STOP lifetime; next() owns each returned frame.
 * Caller must free/transfer frame ownership before reusing its struct.
 * IDs between blocks are observed, not assumed to prove lost line counts.
 * A failed next() must be followed by end(); no resynchronization is attempted.
 */
struct sg_capture_stream {
    struct sg_capture_session *session;
    struct sg_capture_options options;
    struct sg_capture_preflight preflight;
    uint64_t blocks, id_discontinuities;
    uint32_t last_block_id;
    int active, failed, start_attempted, stop_attempted, stop_error, last_discontinuity;
};
int sg_stream_begin(struct sg_capture_stream *, struct sg_capture_session *, const struct sg_capture_options *);
int sg_stream_next(struct sg_capture_stream *, struct sg_capture_frame *);
int sg_stream_end(struct sg_capture_stream *);
void sg_capture_default_options(struct sg_capture_options *);
int sg_capture_session_init(struct sg_capture_session *, const struct sg_capture_io *);
int sg_capture_preflight(struct sg_capture_session *, uint32_t,
                         struct sg_capture_preflight *);
int sg_capture_once(struct sg_capture_session *, const struct sg_capture_options *,
                    struct sg_capture_result *);
void sg_capture_frame_free(struct sg_capture_frame *);
#endif
