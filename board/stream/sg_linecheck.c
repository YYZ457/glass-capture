/* SPDX-License-Identifier: MIT
 *
 * sg_linecheck.c —— ⬜ 待写。纯计算、不碰硬件，适合作为第一个自己从零写的文件。
 * 怎么验证：喂一串假的 block_id（1,2,3,5,6）应得到 id_jumps = 1。
 */
#include "sg_linecheck.h"

void sg_linecheck_reset(struct sg_linecheck *c) { (void)c; /* TODO */ }
int sg_linecheck_feed(struct sg_linecheck *c, const struct sg_capture_frame *f, uint64_t now_ns)
{ (void)c; (void)f; (void)now_ns; return 0; /* TODO */ }
double sg_linecheck_line_rate(const struct sg_linecheck *c) { (void)c; return 0.0; /* TODO */ }
