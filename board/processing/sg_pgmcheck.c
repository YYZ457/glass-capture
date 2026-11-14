/* SPDX-License-Identifier: MIT
 *
 * sg_pgmcheck.c —— 见 sg_pgmcheck.h。一遍扫完整张图（16384 × 2000 = 32 MB 在 A53 上约 0.1–0.3 秒）。
 */
#include <string.h>

#include "sg_pgmcheck.h"
#include "sg_err.h"

const char *sg_ramp_name(int which)
{
    static const char *names[SG_RAMP_COUNT] = {
        "x%256", "(x+start)%256", "x*256/width", "(x+row_start)%256",
    };
    return which >= 0 && which < SG_RAMP_COUNT ? names[which] : "?";
}

static uint8_t percentile(const uint64_t hist[256], uint64_t n, double q)
{
    uint64_t target = (uint64_t)(q * (double)(n - 1)), seen = 0;
    unsigned v;

    for (v = 0; v < 256; ++v) {
        seen += hist[v];
        if (seen > target) return (uint8_t)v;
    }
    return 255;
}

int sg_pgm_check(const uint8_t *img, uint32_t width, uint32_t height, struct sg_pgm_check *out)
{
    uint64_t hist[256], sum = 0, n;
    uint8_t start;
    uint32_t x, y;

    if (!img || !out || width == 0 || height == 0) return -EINVAL;
    memset(out, 0, sizeof *out);
    memset(hist, 0, sizeof hist);
    out->width = width;
    out->height = height;
    start = img[0];

    for (y = 0; y < height; ++y) {
        const uint8_t *row = img + (size_t)y * width;
        uint8_t row_start = row[0];
        for (x = 0; x < width; ++x) {
            uint8_t v = row[x];
            hist[v]++;
            sum += v;
            out->or_tap[x % 3] |= v;
            out->ramp_mismatch[SG_RAMP_X] += v != (uint8_t)x;
            out->ramp_mismatch[SG_RAMP_X_START] += v != (uint8_t)(x + start);
            out->ramp_mismatch[SG_RAMP_SCALED] += v != (uint8_t)((uint64_t)x * 256u / width);
            out->ramp_mismatch[SG_RAMP_ROW_START] += v != (uint8_t)(x + row_start);
        }
    }
    n = (uint64_t)width * height;
    out->or_all = (uint8_t)(out->or_tap[0] | out->or_tap[1] | out->or_tap[2]);
    for (x = 0; x < 256 && !hist[x]; ++x) {}
    out->min = (uint8_t)x;
    for (x = 255; x > 0 && !hist[x]; --x) {}
    out->max = (uint8_t)x;
    out->mean = (double)sum / (double)n;
    out->p1 = percentile(hist, n, 0.01);
    out->p50 = percentile(hist, n, 0.50);
    out->p99 = percentile(hist, n, 0.99);
    out->saturated = hist[255];
    out->all_zero = hist[0] == n;
    out->all_255 = hist[255] == n;
    return 0;
}

int sg_pgm_thumb(const uint8_t *img, uint32_t width, uint32_t height, uint32_t factor,
                 uint8_t *out, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t tw, th, tx, ty, dx, dy;

    if (!img || !out || !out_w || !out_h || factor == 0 || width < factor || height < factor) return -EINVAL;
    tw = width / factor;
    th = height / factor;
    for (ty = 0; ty < th; ++ty) {
        for (tx = 0; tx < tw; ++tx) {
            uint64_t s = 0;
            for (dy = 0; dy < factor; ++dy) {
                const uint8_t *p = img + (size_t)(ty * factor + dy) * width + (size_t)tx * factor;
                for (dx = 0; dx < factor; ++dx) s += p[dx];
            }
            out[(size_t)ty * tw + tx] = (uint8_t)((s + (uint64_t)factor * factor / 2) / ((uint64_t)factor * factor));
        }
    }
    *out_w = tw;
    *out_h = th;
    return 0;
}
