/* SPDX-License-Identifier: MIT
 *
 * sg_linecheck.c —— 连续采集的行记账，纯计算。
 * 怎么验证：喂块号 1,2,3,5,6 应得到 id_jumps = 1。
 */
#include <string.h>

#include "sg_linecheck.h"

void sg_linecheck_reset(struct sg_linecheck *c)
{
    memset(c, 0, sizeof(*c));
}

int sg_linecheck_feed(struct sg_linecheck *c, const struct sg_capture_frame *f, uint64_t now_ns)
{
    int jump = 0;

    /* 第一块没有"上一块"可比；之后块号必须正好 +1（uint32 自然回绕也算连续） */
    if (c->blocks && f->block_id != (uint32_t)(c->last_block_id + 1u)) {
        c->id_jumps++;
        jump = 1;
    }
    if (c->blocks == 0) {
        c->first_ns = now_ns;
        c->first_height = f->height;
    }
    c->last_block_id = f->block_id;
    c->last_ns = now_ns;
    c->blocks++;
    c->lines += f->height;
    return jump;
}

double sg_linecheck_line_rate(const struct sg_linecheck *c)
{
    /* 第一块是在 first_ns 之前拍的，没法计时，所以分子去掉第一块的行数。 */
    if (c->blocks < 2 || c->last_ns <= c->first_ns) {
        return 0.0;
    }
    return (double)(c->lines - c->first_height) * 1e9 / (double)(c->last_ns - c->first_ns);
}
