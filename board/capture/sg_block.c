/* SPDX-License-Identifier: MIT
 *
 * sg_block.c —— 块头读取、校验、分块 DMA 收数。
 * 职责：把"卡上 DDR 里的一个块"变成"内存里的一段连续像素"。
 * 依赖：sg_session（rd/wr/dma_read/now/sleep）、sg3000_regs.h 的 SG_HDR_*
 * 被谁用：sg_acq.c
 *
 * 迁移来源：legacy sg3000_capture_core.c
 *   sg_block_header_read   ← 第 108-134 行 get_header()（h[] 下标 → 结构体字段）
 *   sg_block_header_check  ← 第 193-209 行 receive_block() 里的校验条件，单独拎出来
 *   sg_block_receive       ← 第 183-242 行 receive_block() 其余部分
 * 注意：块大小 capacity = width × max_lines，由第一个包决定，后续包的
 *       block_id / width / max_lines 必须相同。
 * 怎么验证：
 *   1. 电脑上：用 2026-10-30 录下的真实包头做 header_check 单元测试
 *   2. 板上：采 1 块，length 应 = width × lines = 8,192,000
 */
#include "sg_block.h"
#include "sg3000_regs.h"
#include "sg_err.h"

int sg_block_header_read(struct sg_capture_session *s, uint64_t deadline_ms,
                         struct sg_block_header *h)
{ (void)s; (void)deadline_ms; (void)h; return -ENOSYS; /* TODO */ }

int sg_block_header_check(const struct sg_block_header *h, uint32_t expect_seq,
                          uint64_t filled)
{ (void)h; (void)expect_seq; (void)filled; return -ENOSYS; /* TODO */ }

int sg_block_receive(struct sg_capture_session *s, const struct sg_capture_options *o,
                     struct sg_capture_frame *frame)
{ (void)s; (void)o; (void)frame; return -ENOSYS; /* TODO */ }
