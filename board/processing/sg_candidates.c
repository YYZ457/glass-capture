/* SPDX-License-Identifier: MIT
 *
 * sg_candidates.c —— 颗粒候选检测，算法见 sg_candidates.h。
 *
 * 连通域用"逐行扫描 + 并查集"：只保留上一行和当前行的标号，内存只和宽度有关，
 * 不需要整张图大小的标号数组。每个标号带一份统计（面积、外接框、加权和），
 * 两个标号合并时统计也合并到根上。
 *
 * 状态：🟡 电脑上合成图测过；算法本身还没对过真值（verify/V4）
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sg_candidates.h"
#include "sg_err.h"

struct comp {
    uint32_t parent;
    uint32_t x0, y0, x1, y1, area;
    float peak;
    double w, wx, wy;           /* 权重和、x 加权和、y 加权和 */
};

struct labels {
    struct comp *c;
    uint32_t used, cap;         /* c[0] 不用，标号从 1 开始 */
};

static uint32_t find(struct labels *L, uint32_t i)
{
    while (L->c[i].parent != i) {
        L->c[i].parent = L->c[L->c[i].parent].parent;   /* 路径减半 */
        i = L->c[i].parent;
    }
    return i;
}

/* 合并两个标号，小号做根（小号先出现，保证输出顺序）。返回根。 */
static uint32_t unite(struct labels *L, uint32_t a, uint32_t b)
{
    struct comp *ra, *rb;

    a = find(L, a);
    b = find(L, b);
    if (a == b) {
        return a;
    }
    if (a > b) {
        uint32_t t = a; a = b; b = t;
    }
    ra = &L->c[a];
    rb = &L->c[b];
    rb->parent = a;
    if (rb->x0 < ra->x0) ra->x0 = rb->x0;
    if (rb->y0 < ra->y0) ra->y0 = rb->y0;
    if (rb->x1 > ra->x1) ra->x1 = rb->x1;
    if (rb->y1 > ra->y1) ra->y1 = rb->y1;
    if (rb->peak > ra->peak) ra->peak = rb->peak;
    ra->area += rb->area;
    ra->w  += rb->w;
    ra->wx += rb->wx;
    ra->wy += rb->wy;
    return a;
}

static int new_label(struct labels *L, uint32_t *id)
{
    if (L->used + 1 >= L->cap) {
        uint32_t cap = L->cap ? L->cap * 2 : 1024;
        struct comp *c;

        if (cap > SG_CANDIDATES_MAX_LABELS) {
            cap = SG_CANDIDATES_MAX_LABELS;
        }
        if (L->used + 1 >= cap) {
            return -E2BIG;
        }
        c = realloc(L->c, (size_t)cap * sizeof(*c));
        if (c == NULL) {
            return -ENOMEM;
        }
        L->c = c;
        L->cap = cap;
    }
    *id = ++L->used;
    memset(&L->c[*id], 0, sizeof(L->c[*id]));
    L->c[*id].parent = *id;
    L->c[*id].x0 = L->c[*id].y0 = UINT32_MAX;
    return 0;
}

static void add_pixel(struct comp *c, uint32_t x, uint32_t y, float amp)
{
    if (x < c->x0) c->x0 = x;
    if (y < c->y0) c->y0 = y;
    if (x + 1 > c->x1) c->x1 = x + 1;
    if (y + 1 > c->y1) c->y1 = y + 1;
    if (amp > c->peak) c->peak = amp;
    c->area++;
    c->w  += amp;
    c->wx += (double)amp * x;
    c->wy += (double)amp * y;
}

/* 没给背景模型时：用这一块自己的逐列均值。 */
static float *self_background(const struct sg_image_view *v)
{
    float *mean = malloc((size_t)v->width * sizeof(*mean));
    double *sum = calloc(v->width, sizeof(*sum));
    uint32_t x, y;

    if (mean == NULL || sum == NULL) {
        free(mean);
        free(sum);
        return NULL;
    }
    for (y = 0; y < v->height; ++y) {
        const unsigned char *row = v->pixels + (size_t)y * v->stride;
        for (x = 0; x < v->width; ++x) {
            sum[x] += row[x];
        }
    }
    for (x = 0; x < v->width; ++x) {
        mean[x] = (float)(sum[x] / v->height);
    }
    free(sum);
    return mean;
}

static int params_valid(const struct sg_candidate_params *p)
{
    return p->threshold >= 1 && p->threshold <= 255 && p->min_area >= 1 &&
           (p->max_area == 0 || p->max_area >= p->min_area);
}

int sg_candidates_find(const struct sg_image_view *v, const struct sg_background *bg,
                       const struct sg_candidate_params *p,
                       struct sg_candidate *out, uint32_t capacity, uint32_t *count)
{
    struct labels L = { NULL, 0, 0 };
    uint32_t *prev = NULL, *cur = NULL;
    float *own_mean = NULL;
    const float *mean;
    uint32_t x, y, i, limit;
    int ret = 0, truncated = 0;

    if (count == NULL || p == NULL || (out == NULL && capacity != 0)) {
        return -EINVAL;
    }
    *count = 0;
    ret = sg_view_validate(v);
    if (ret) {
        return ret;
    }
    if (!params_valid(p)) {
        return -EINVAL;
    }

    /* ① 背景 */
    if (bg != NULL) {
        if (bg->width != v->width || bg->column_mean == NULL) {
            return -EINVAL;
        }
        mean = bg->column_mean;
    } else {
        own_mean = self_background(v);
        if (own_mean == NULL) {
            return -ENOMEM;
        }
        mean = own_mean;
    }

    /* ② ③ 逐行：阈值 + 连通域。prev/cur 两行标号，0 = 不可疑。 */
    prev = calloc(v->width, sizeof(*prev));
    cur  = calloc(v->width, sizeof(*cur));
    if (prev == NULL || cur == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    for (y = 0; y < v->height; ++y) {
        const unsigned char *row = v->pixels + (size_t)y * v->stride;
        uint32_t *swap;

        for (x = 0; x < v->width; ++x) {
            float amp = fabsf((float)row[x] - mean[x]);
            uint32_t label = 0, n[4];
            int k;

            cur[x] = 0;
            if (amp < (float)p->threshold) {
                continue;
            }

            /* 已经扫过的 4 个邻居：左、左上、上、右上 */
            n[0] = x > 0 ? cur[x - 1] : 0;
            n[1] = x > 0 ? prev[x - 1] : 0;
            n[2] = prev[x];
            n[3] = x + 1 < v->width ? prev[x + 1] : 0;
            for (k = 0; k < 4; ++k) {
                if (n[k]) {
                    label = label ? unite(&L, label, n[k]) : find(&L, n[k]);
                }
            }
            if (!label) {
                ret = new_label(&L, &label);
                if (ret) {
                    goto out;
                }
            }
            add_pixel(&L.c[label], x, y, amp);
            cur[x] = label;
        }
        swap = prev; prev = cur; cur = swap;
    }

    /* ④ 输出：只看根，按标号从小到大（= 第一个像素的光栅顺序） */
    limit = capacity;
    if (p->max_candidates && p->max_candidates < limit) {
        limit = p->max_candidates;
    }
    for (i = 1; i <= L.used; ++i) {
        const struct comp *c = &L.c[i];
        struct sg_candidate *o;

        if (c->parent != i || c->area < p->min_area ||
            (p->max_area && c->area > p->max_area)) {
            continue;
        }
        if (*count >= limit) {
            truncated = 1;
            break;
        }
        o = &out[(*count)++];
        o->x0 = c->x0; o->y0 = c->y0; o->x1 = c->x1; o->y1 = c->y1;
        o->area = c->area;
        o->peak = (uint32_t)lroundf(c->peak);
        o->cx = c->w > 0 ? (float)(c->wx / c->w) : (float)c->x0;
        o->cy = c->w > 0 ? (float)(c->wy / c->w) : (float)c->y0;
    }
    ret = truncated ? SG_CANDIDATES_TRUNCATED : 0;

out:
    free(L.c);
    free(prev);
    free(cur);
    free(own_mean);
    return ret;
}
