/* SPDX-License-Identifier: MIT
 *
 * sg_block.h —— 一个数据块的"包头"：读、查、按包头把数据 DMA 回来。
 *
 * legacy 里块头是一个 uint32_t h[23] 数组，靠下标访问（h[4] 是宽、h[17] 是块号……）。
 * 这里改成有名字的结构体，下标对照见 docs/interfaces/I3_block_header.md。
 * 寄存器读取顺序必须照搬 Windows（先写 SG_HDR_ACK，再按固定顺序读 20 个寄存器），
 * 顺序表在 legacy core.c 第 116-117 行。
 *
 * 状态：✅ 字段校验 2026-10-30 实测通过（4096×2000，1 包）
 */
#ifndef SG_BLOCK_H
#define SG_BLOCK_H
#include <stdint.h>
#include "sg_session.h"

struct sg_block_header {
    uint32_t width;        /* h[4]  SG_HDR_WIDTH */
    uint32_t lines;        /* h[5]  SG_HDR_LINES，最后一包时 = 本块行数 */
    uint32_t pixfmt;       /* h[6]  必须 = SG_PIXFMT_MONO8 */
    uint32_t zero_a, zero_b;/* h[7] h[8] 必须为 0 */
    uint32_t seq;          /* h[9]  bit30..0 包序号，bit31 最后一包 */
    uint32_t payload_len;  /* h[10] */
    uint32_t dest_offset;  /* h[11] */
    uint32_t src_addr;     /* h[15] 卡上 DDR 地址 */
    uint32_t block_id;     /* h[17] */
    uint32_t magic;        /* h[21] 必须 = SG_HDR_MAGIC_VALUE */
    uint32_t max_lines;    /* h[22] */
    uint32_t opaque[8];    /* h[2] h[3] h[12] h[13] h[16] h[18] h[19] h[20]：读了但不懂 */
};
#define SG_SEQ_LAST  0x80000000u
#define SG_SEQ_MASK  0x7fffffffu

/* 等到 SG_HDR_READY 非 0（或超过 deadline_ms），然后按 Windows 顺序读出包头。 */
int sg_block_header_read(struct sg_capture_session *s, uint64_t deadline_ms,
                         struct sg_block_header *h);

/* 纯函数，不碰硬件：检查一个包头本身和它与前面包的一致性。
 * expect_seq = 期望的包序号（从 1 起），filled = 已收字节数。
 * 不通过返回 -EPROTO。最适合先在电脑上写单元测试。 */
int sg_block_header_check(const struct sg_block_header *h, uint32_t expect_seq,
                          uint64_t filled);

/* 收完整一个块：循环 读包头 → 检查 → 分 64 KB DMA，直到最后一包。
 * 成功时 frame->data 由本函数 malloc，所有权交给调用者；失败时自动释放。 */
int sg_block_receive(struct sg_capture_session *s, const struct sg_capture_options *o,
                     struct sg_capture_frame *frame);

#endif
