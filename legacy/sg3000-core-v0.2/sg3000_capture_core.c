/* SPDX-License-Identifier: MIT
 * Experimental port-0 protocol core. No filesystem or OS/hardware access here.
 */
#include "sg3000_capture_core.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FIFO 0x412000u
#define MONO8 0x01080001u
#define ACQ_START 0x10601014u
#define ACQ_STOP 0x10601018u
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const unsigned char *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(unsigned char *p, uint32_t value)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i)); }
static int cancelled(struct sg_capture_session *s)
{ return s->io.cancelled && s->io.cancelled(s->io.context); }
static int rd(struct sg_capture_session *s, uint32_t off, uint32_t *v)
{ return s->io.read_reg(s->io.context, off, v); }
static int wr(struct sg_capture_session *s, uint32_t off, uint32_t v)
{ return s->io.write_reg(s->io.context, off, v); }

void sg_capture_default_options(struct sg_capture_options *o)
{
    o->control_timeout_ms = 1000; o->frame_timeout_ms = 10000;
    o->dma_timeout_ms = 1000; o->max_frame_bytes = SG_CAPTURE_MAX_BYTES;
}
int sg_capture_session_init(struct sg_capture_session *s, const struct sg_capture_io *io)
{
    if (!s || !io || !io->read_reg || !io->write_reg || !io->dma_read ||
        !io->now_ms || !io->sleep_ms) return -EINVAL;
    memset(s, 0, sizeof(*s)); s->io = *io; return 0;
}
void sg_capture_frame_free(struct sg_capture_frame *frame)
{ if (frame) { free(frame->data); memset(frame, 0, sizeof(*frame)); } }

/* Requests and ACKs have independent counters. Never require an ACK to echo
 * the request number. Do require a continuous ACK counter within this session.
 * Stop cleanup ignores cancellation but still has its own bounded deadline.
 */
static int logical(struct sg_capture_session *s, uint32_t address, int write,
                   uint32_t *value, uint32_t timeout, int cleanup)
{
    uint32_t occupancy, room, packet[8] = {0x000155aa, 0, 0, 0, 0, 0, 4, 1};
    unsigned char reply[600];
    uint32_t length, word; unsigned i, words = write ? 8u : 7u;
    uint64_t deadline = s->io.now_ms(s->io.context) + timeout;
    uint16_t ack; int ret;
    if (!cleanup && cancelled(s)) return -ECANCELED;
    if (write && address != ACQ_START && address != ACQ_STOP) return -EPERM;
    ret = rd(s, FIFO + 0x1c, &occupancy); if (ret) return ret;
    if (occupancy) return -EBUSY; /* No reset, drain, or automatic retry. */
    ret = rd(s, FIFO + 0x0c, &room); if (ret) return ret;
    if (room < words) return -ENOSPC;
    packet[2] = (write ? 16u : 12u) | ((uint32_t)++s->request_id << 16);
    packet[4] = write ? 0x80020000u : 0x80000000u; packet[5] = address;
    for (i = 0; i < words; ++i) { ret = wr(s, FIFO + 0x10, packet[i]); if (ret) return ret; }
    ret = wr(s, FIFO + 0x14, words * 4u); if (ret) return ret;
    for (;;) {
        if (!cleanup && cancelled(s)) return -ECANCELED;
        if (s->io.now_ms(s->io.context) >= deadline) return -ETIMEDOUT;
        ret = rd(s, FIFO + 0x1c, &occupancy); if (ret) return ret;
        if (occupancy) break;
        s->io.sleep_ms(s->io.context, 1);
    }
    ret = rd(s, FIFO + 0x24, &length); if (ret) return ret;
    if (length < 28 || length > sizeof(reply) || (length & 3u)) return -EPROTO;
    for (i = 0; i < length; i += 4) {
        ret = rd(s, FIFO + 0x20, &word); if (ret) return ret; put32(reply + i, word);
    }
    if (length != (write ? 28u : 32u) || le32(reply) != 0x000055aau ||
        le32(reply + 4) != 1u ||
        le16(reply + 8) != (write ? 12u : 16u) || le16(reply + 16) != 0 ||
        le16(reply + 18) != (write ? 0x8003u : 0x8001u) ||
        le16(reply + 24) != (write ? 0u : 4u)) return -EPROTO;
    ack = le16(reply + 10);
    if (s->have_ack && ack != (uint16_t)(s->last_ack + 1u)) return -EPROTO;
    s->have_ack = 1; s->last_ack = ack;
    if (!write) *value = le32(reply + 28);
    return 0;
}

int sg_capture_preflight(struct sg_capture_session *s, uint32_t timeout,
                         struct sg_capture_preflight *p)
{
    int ret;
    if (!s || !p || !timeout || timeout > 2000) return -EINVAL;
    memset(p, 0, sizeof(*p));
    ret = logical(s, 0x10540024u, 0, &p->ddr_status, timeout, 0); if (ret) return ret;
    p->valid_mask |= 1u;
    ret = logical(s, 0x10002020u, 0, &p->pixel_clocks, timeout, 0); if (ret) return ret;
    p->valid_mask |= 2u;
    /* DDRInitialStatus 0 (uninitialized) or 2 (calibrated/ready) are normal in hardware.
     * Current XML Clock1 is bit 0. Other active links cannot satisfy port 0. */
    if ((p->ddr_status != 0 && p->ddr_status != 2) || !(p->pixel_clocks & 1u)) return -ENOLINK;
    ret = logical(s, 0x10601004u, 0, &p->cl_configuration, timeout, 0); if (ret) return ret;
    p->valid_mask |= 4u;
    ret = logical(s, 0x10601008u, 0, &p->camera_color, timeout, 0); if (ret) return ret;
    p->valid_mask |= 8u;
    ret = logical(s, 0x1060100cu, 0, &p->camera_depth, timeout, 0); if (ret) return ret;
    p->valid_mask |= 16u;
    if (p->cl_configuration || p->camera_color || p->camera_depth != 8u) return -ENOTSUP;
    return 0;
}

static int get_header(struct sg_capture_session *s, uint64_t deadline, uint32_t *h)
{
    /* -1 means synthetic, not an MMIO read. Unknown semantics remain opaque. */
    static const int offsets[23] = {-1, -1, 0x202028, 0x20202c,
        0x202040, 0x202044, 0x202050, 0x202048, 0x20204c,
        0x20201c, 0x202018, 0x202020, 0x202024, 0x202008,
        -1, 0x202010, 0x202014, 0x202030, 0x202034,
        0x202038, 0x20203c, 0x202098, 0x202068};
    static const unsigned order[20] = {15,16,17,18,4,7,8,19,20,5,6,13,
                                        9,10,11,12,2,3,22,21};
    uint32_t ready; int ret; unsigned i;
    for (;;) {
        if (cancelled(s)) return -ECANCELED;
        if (s->io.now_ms(s->io.context) >= deadline) return -ETIMEDOUT;
        ret = rd(s, 0x20206cu, &ready); if (ret) return ret;
        if (ready) break;
        s->io.sleep_ms(s->io.context, 1);
    }
    /* Match the supplied Windows port-0 sequence: ACK, then copy registers. */
    ret = wr(s, 0x202000u, UINT32_MAX); if (ret) return ret;
    memset(h, 0, 92); h[1] = 1;
    for (i = 0; i < 20; ++i) {
        unsigned field = order[i];
        ret = rd(s, (uint32_t)offsets[field], &h[field]); if (ret) return ret;
    }
    return 0;
}

static int stop_capture(struct sg_capture_session *s, uint32_t timeout)
{
    uint32_t state; int ret;
    uint64_t deadline;
    ret = logical(s, ACQ_STOP, 1, NULL, timeout, 1); if (ret) return ret;
    deadline = s->io.now_ms(s->io.context) + timeout;
    for (;;) {
        if (s->io.now_ms(s->io.context) >= deadline) return -ETIMEDOUT;
        ret = rd(s, 0x20209cu, &state); if (ret) return ret;
        if (state == 0x53544f50u) return 0;
        s->io.sleep_ms(s->io.context, 1);
    }
}

int sg_stream_begin(struct sg_capture_stream *stream, struct sg_capture_session *s,
                    const struct sg_capture_options *o)
{
    uint32_t ready; int ret;
    if (!stream) return -EINVAL;
    memset(stream, 0, sizeof(*stream));
    if (!s || !o || !o->control_timeout_ms || o->control_timeout_ms > 2000 ||
        !o->frame_timeout_ms || o->frame_timeout_ms > 60000 ||
        !o->dma_timeout_ms || o->dma_timeout_ms > 2000 ||
        !o->max_frame_bytes || o->max_frame_bytes > SG_CAPTURE_MAX_BYTES) return -EINVAL;
    stream->session = s; stream->options = *o;
    ret = sg_capture_preflight(s, o->control_timeout_ms, &stream->preflight);
    if (ret) return ret;
    if (cancelled(s)) return -ECANCELED;
    ret = rd(s, 0x20206cu, &ready); if (ret) return ret;
    if (ready) return -EBUSY;
    /* Even a failed START write needs bounded STOP cleanup. */
    stream->start_attempted = 1;
    ret = wr(s, 0x201000u, 2); if (ret) return ret;
    ret = logical(s, ACQ_START, 1, NULL, o->control_timeout_ms, 0);
    if (!ret) stream->active = 1;
    return ret;
}

int sg_stream_end(struct sg_capture_stream *stream)
{
    if (!stream) return -EINVAL;
    if (!stream->start_attempted || stream->stop_attempted) return stream->stop_error;
    stream->stop_attempted = 1; stream->active = 0;
    stream->stop_error = stop_capture(stream->session, stream->options.control_timeout_ms);
    return stream->stop_error;
}

static int receive_block(struct sg_capture_session *s, const struct sg_capture_options *o,
                         struct sg_capture_frame *frame)
{
    uint64_t deadline, capacity = 0, end, source_end;
    uint32_t h[23], block = 0, width = 0, max_height = 0, sequence = 1;
    size_t filled = 0; unsigned packets = 0; int ret, completed = 0;
    deadline = s->io.now_ms(s->io.context) + o->frame_timeout_ms;
    while (!completed) {
        uint32_t payload_done = 0;
        ret = get_header(s, deadline, h); if (ret) goto done;
        if (h[21] != 0x53434255u || !h[17] || !(h[9] & 0x7fffffffu) ||
            !h[4] || !h[22] || h[5] > h[22] || h[6] != MONO8 || h[7] || h[8] ||
            !h[10] || (h[9] & 0x7fffffffu) != sequence || h[11] != filled) {
            ret = -EPROTO; goto done;
        }
        end = (uint64_t)h[11] + h[10];
        source_end = (uint64_t)h[15] + h[10];
        if (!packets) {
            capacity = (uint64_t)h[4] * h[22];
            if (capacity > o->max_frame_bytes) { ret = -EFBIG; goto done; }
            block = h[17]; width = h[4]; max_height = h[22];
        } else if (block != h[17] || width != h[4] || max_height != h[22]) {
            ret = -EPROTO; goto done;
        }
        if (end > capacity || source_end > (UINT64_C(1) << 32) ||
            ((h[9] & 0x80000000u) && (!h[5] || end != (uint64_t)width * h[5]))) {
            ret = -EPROTO; goto done;
        }
        if (!frame->data) {
            frame->data = malloc((size_t)capacity);
            if (!frame->data) { ret = -ENOMEM; goto done; }
        }
        while (payload_done < h[10]) {
            uint32_t length = h[10] - payload_done, timeout;
            uint64_t now = s->io.now_ms(s->io.context), remaining;
            if (cancelled(s)) { ret = -ECANCELED; goto done; }
            if (now >= deadline) { ret = -ETIMEDOUT; goto done; }
            remaining = deadline - now;
            if (length > SG_CAPTURE_DMA_CHUNK) length = SG_CAPTURE_DMA_CHUNK;
            timeout = o->dma_timeout_ms;
            if (remaining < timeout) timeout = (uint32_t)remaining;
            ret = s->io.dma_read(s->io.context, h[15] + payload_done,
                                frame->data + filled + payload_done, length, timeout);
            if (ret) goto done;
            payload_done += length;
        }
        filled = (size_t)end; ++packets; ++sequence;
        completed = (h[9] & 0x80000000u) != 0;
        if (completed) {
            if (cancelled(s)) { ret = -ECANCELED; goto done; }
            if (s->io.now_ms(s->io.context) >= deadline) { ret = -ETIMEDOUT; goto done; }
            frame->length = filled; frame->width = width; frame->height = h[5];
            frame->block_id = block; frame->packets = packets;
        }
    }
    ret = 0;
done:
    if (ret) sg_capture_frame_free(frame);
    return ret;
}

int sg_stream_next(struct sg_capture_stream *stream, struct sg_capture_frame *frame)
{
    int ret;
    if (!stream || !frame || !stream->active || stream->failed) return -EINVAL;
    memset(frame, 0, sizeof(*frame));
    ret = receive_block(stream->session, &stream->options, frame);
    if (ret) { stream->failed = 1; return ret; }
    stream->last_discontinuity = stream->blocks &&
        frame->block_id != (uint32_t)(stream->last_block_id + 1u);
    if (stream->last_discontinuity) ++stream->id_discontinuities;
    stream->last_block_id = frame->block_id;
    ++stream->blocks;
    return 0;
}

int sg_capture_once(struct sg_capture_session *s, const struct sg_capture_options *o,
                    struct sg_capture_result *r)
{
    struct sg_capture_stream stream; int ret;
    if (!r) return -EINVAL;
    memset(r, 0, sizeof(*r));
    ret = sg_stream_begin(&stream, s, o);
    r->preflight = stream.preflight;
    if (!ret) ret = sg_stream_next(&stream, &r->frame);
    r->primary_error = ret;
    r->stop_error = sg_stream_end(&stream);
    r->start_attempted = stream.start_attempted;
    r->stop_attempted = stream.stop_attempted;
    if (ret || r->stop_error) sg_capture_frame_free(&r->frame);
    return ret ? ret : r->stop_error;
}
