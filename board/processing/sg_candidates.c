/* SPDX-License-Identifier: MIT
 *
 * sg_candidates.c —— ⬜ 待写。先完成 host/ 侧的 Python 版本和 verify/V4 真值比对。
 */
#include "sg_candidates.h"
#include "sg_err.h"

int sg_candidates_find(const struct sg_image_view *v, const struct sg_background *bg,
                       const struct sg_candidate_params *p,
                       struct sg_candidate *out, uint32_t capacity, uint32_t *count)
{ (void)v; (void)bg; (void)p; (void)out; (void)capacity; (void)count;
  return -ENOSYS; /* TODO */ }
