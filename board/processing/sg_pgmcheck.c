/* SPDX-License-Identifier: MIT
 *
 * sg_pgmcheck.c —— 见 sg_pgmcheck.h。一遍扫完整张图（16384 × 2000 = 32 MB 在 A53 上约 0.1–0.3 秒）。
 */
#include <stdlib.h>
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

/* 档位边界：暗处分得细（现在的实拍图最大灰度只有十几），亮处分得粗。 */
static const uint32_t noise_edges[SG_NOISE_BINS + 1] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 160, 192, 224, 256,
};

int sg_pgm_noise(const uint8_t *img, uint32_t width, uint32_t height, struct sg_pgm_noise *out)
{
    uint64_t *sum, *sq, *dsq;
    uint8_t *sat;
    uint32_t tap_n[3] = { 0, 0, 0 }, x, y, b;
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;

    if (!img || !out || width == 0 || height < 2) return -EINVAL;
    memset(out, 0, sizeof *out);
    sum = calloc(width, sizeof *sum);
    sq = calloc(width, sizeof *sq);
    dsq = calloc(width, sizeof *dsq);
    sat = calloc(width, 1);
    if (!sum || !sq || !dsq || !sat) {
        free(sum); free(sq); free(dsq); free(sat);
        return -ENOMEM;
    }
    for (y = 0; y < height; ++y) {
        const uint8_t *row = img + (size_t)y * width;
        const uint8_t *next = y + 1 < height ? row + width : NULL;
        for (x = 0; x < width; ++x) {
            uint32_t v = row[x];
            sum[x] += v;
            sq[x] += v * v;
            sat[x] |= v == 255;
            if (next) {
                int32_t d = (int32_t)next[x] - (int32_t)v;
                dsq[x] += (uint32_t)(d * d);
            }
        }
    }
    for (b = 0; b < SG_NOISE_BINS; ++b) {
        out->bin[b].lo = noise_edges[b];
        out->bin[b].hi = noise_edges[b + 1];
    }
    for (x = 0; x < width; ++x) {
        double mean = (double)sum[x] / height;
        double total = (double)sq[x] / height - mean * mean;
        double diff = (double)dsq[x] / (2.0 * (height - 1));
        struct sg_noise_bin *bin;

        if (sat[x]) {
            out->saturated_columns++;
            continue;
        }
        for (b = 0; b + 1 < SG_NOISE_BINS && mean >= noise_edges[b + 1]; ++b) {}
        bin = &out->bin[b];
        bin->columns++;
        bin->mean += mean;
        bin->var_diff += diff;
        bin->var_total += total > 0 ? total : 0;
        out->tap_mean[x % 3] += mean;
        out->tap_var[x % 3] += diff;
        tap_n[x % 3]++;
    }
    for (b = 0; b < 3; ++b) {
        if (tap_n[b]) {
            out->tap_mean[b] /= tap_n[b];
            out->tap_var[b] /= tap_n[b];
        }
    }
    /* 按列数加权的最小二乘直线 */
    for (b = 0; b < SG_NOISE_BINS; ++b) {
        struct sg_noise_bin *bin = &out->bin[b];
        if (!bin->columns) continue;
        bin->mean /= bin->columns;
        bin->var_diff /= bin->columns;
        bin->var_total /= bin->columns;
        if (bin->columns >= 16) {
            double w = bin->columns;
            sw += w; sx += w * bin->mean; sy += w * bin->var_diff;
            sxx += w * bin->mean * bin->mean; sxy += w * bin->mean * bin->var_diff;
            out->fit_bins++;
        }
    }
    if (out->fit_bins >= 3) {
        double den = sw * sxx - sx * sx;
        if (den > 1e-9 * sw * sxx) {
            out->slope = (sw * sxy - sx * sy) / den;
            out->read_var = (sy - out->slope * sx) / sw;
            out->fit_ok = out->slope > 0;
        }
    }
    free(sum); free(sq); free(dsq); free(sat);
    return 0;
}
