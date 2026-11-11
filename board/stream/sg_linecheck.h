/* SPDX-License-Identifier: MIT
 *
 * sg_linecheck.h —— 连续采集时的行完整性记账。
 *
 * 回答的问题："我收到的行，是不是相机真正拍下的全部行，且顺序正确？"
 * 数据来源：每块的 block_id、height，以及（将来接上编码器后）的物理位置。
 * 现在能做的：块号跳变计数、累计行数、每块用时（推算实际行频）。
 * 将来能做的：和编码器脉冲数对账，得出真正的丢行数。
 */
#ifndef SG_LINECHECK_H
#define SG_LINECHECK_H
#include <stdint.h>
#include "sg_frame.h"

struct sg_linecheck {
    uint64_t blocks, lines, id_jumps;
    uint32_t last_block_id, first_height;
    uint64_t first_ns, last_ns;     /* 平均行频 = (lines - first_height) / (last_ns - first_ns) */
};

void sg_linecheck_reset(struct sg_linecheck *c);
/* 每收到一块调一次。返回 1 表示这一块和上一块块号不连续。 */
int sg_linecheck_feed(struct sg_linecheck *c, const struct sg_capture_frame *f, uint64_t now_ns);
/* 平均行频（Hz），数据不足时返回 0。 */
double sg_linecheck_line_rate(const struct sg_linecheck *c);

#endif
