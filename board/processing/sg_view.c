/* SPDX-License-Identifier: MIT
 *
 * sg_view.c —— 检查一个 sg_image_view 是否合法。所有处理器动像素之前先调它，防止越界读。
 * 迁移来源：legacy sg_processing.c 第 9-16 行
 */
#include "sg_processing.h"
#include "sg_err.h"

int sg_view_validate(const struct sg_image_view *v)
{
    if (v == NULL || v->pixels == NULL) {
        return -EINVAL;
    }
    if (v->width == 0 || v->height == 0 || v->stride < v->width) {
        return -EINVAL;
    }
    if (v->pixel_format != SG_PIXEL_MONO8) {
        return -EINVAL;
    }
    /* 最后一行也要完整地落在缓冲区里 */
    if (v->bytes < (uint64_t)v->stride * v->height || v->bytes > SG_CAPTURE_MAX_BYTES) {
        return -EINVAL;
    }
    return 0;
}
