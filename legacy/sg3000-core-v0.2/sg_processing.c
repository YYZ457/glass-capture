/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sg_processing.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sg_view_validate(const struct sg_image_view *v)
{
    if (!v || !v->pixels || !v->width || !v->height || v->stride < v->width ||
        v->pixel_format != SG_PIXEL_MONO8 ||
        v->bytes < (uint64_t)v->stride * v->height || v->bytes > SG_CAPTURE_MAX_BYTES)
        return -EINVAL;
    return 0;
}
static uint64_t ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int stats(void *context, const struct sg_image_view *v, uint32_t timeout,
                 struct sg_processing_result *r)
{
    uint32_t x, y, threshold = *(uint32_t *)context;
    uint64_t deadline = ms() + timeout;
    int ret = sg_view_validate(v);
    if (ret || !r || !timeout || timeout > 60000) return ret ? ret : -EINVAL;
    memset(r, 0, sizeof(*r)); r->abi = SG_PROCESS_ABI; r->algorithm = 1;
    r->sequence = v->sequence; r->first_line = v->first_line;
    r->minimum = 255; r->pixel_count = (uint64_t)v->width * v->height;
    /* This is a diagnostic measurement, NEVER a glass defect classifier. */
    r->decision = SG_UNKNOWN;
    for (y = 0; y < v->height; ++y) {
        if (ms() >= deadline) return -ETIMEDOUT;
        for (x = 0; x < v->width; ++x) {
            unsigned value = v->pixels[(size_t)y * v->stride + x];
            r->sum += value; r->above_threshold += value > threshold;
            if (value < r->minimum) r->minimum = value;
            if (value > r->maximum) r->maximum = value;
        }
    }
    return 0;
}
static void stats_close(void *context) { free(context); }
int sg_stats_open(struct sg_processor *p, uint32_t threshold)
{
    uint32_t *setting;
    if (!p || threshold > 255) return -EINVAL;
    setting = malloc(sizeof(*setting)); if (!setting) return -ENOMEM;
    *setting = threshold; p->context = setting; p->process = stats; p->close = stats_close;
    return 0;
}
