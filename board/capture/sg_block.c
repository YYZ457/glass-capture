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
 *
 * 一个"块"在卡上可能分成若干"包"。每个包有自己的包头，说明：
 * 这包有多少字节、放在整块的哪个位置、在卡 DDR 的哪个地址。
 * 收一个块 = 反复"读包头 → 检查 → DMA 搬数据"，直到收到标着"最后一包"的那个包。
 *
 * 校验拆成两部分，写法都和 legacy 一致：
 *   sg_block_header_check  只看这一个包头本身，以及它的包序号、位置是否接得上（纯函数）
 *   sg_block_receive       还要跟"前面几个包"比较（块号、宽度是否一致）、
 *                          不超出整块容量，这些需要状态，所以留在 receive 里
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sg_block.h"
#include "sg3000_regs.h"
#include "sg3000_rx_uapi.h"     /* SG_DMA_CHUNK：每次 DMA 最多搬多少字节 */
#include "sg_err.h"

/* ==== 读包头 =========================================================== */

/* 读包头的 20 个寄存器的顺序，照搬 Windows 软件的顺序（legacy 第 116-117 行）。
 * 其中 8 个含义不明，只是照读，存进 opaque[]（地址见 sg3000_regs.h 的 [未知] 列表）。
 *
 * 下面这张表每行是 { 寄存器地址, 读到的值存到哪里 }。 */
struct hdr_step {
    uint32_t reg;
    uint32_t *dst;
};

int sg_block_header_read(struct sg_capture_session *s, uint64_t deadline_ms,
                         struct sg_block_header *h)
{
    uint32_t ready;
    unsigned i;
    int ret;

    /* ① 等"数据块就绪"，期间检查取消和超时。 */
    for (;;) {
        if (sg_session_cancelled(s)) {
            return -ECANCELED;
        }
        if (s->io.now_ms(s->io.context) >= deadline_ms) {
            return -ETIMEDOUT;
        }

        ret = sg_session_rd(s, SG_HDR_READY, &ready);
        if (ret) {
            return ret;
        }
        if (ready) {
            break;
        }
        s->io.sleep_ms(s->io.context, 1);
    }

    /* ② 照搬 Windows 的顺序：先"确认"，再依次拷贝各寄存器。 */
    ret = sg_session_wr(s, SG_HDR_ACK, SG_HDR_ACK_VALUE);
    if (ret) {
        return ret;
    }

    memset(h, 0, sizeof(*h));

    {
        const struct hdr_step steps[20] = {
            { SG_HDR_SRC_ADDR,    &h->src_addr    },
            { 0x202014u,          &h->opaque[4]   },   /* [未知] */
            { SG_HDR_BLOCK_ID,    &h->block_id    },
            { 0x202034u,          &h->opaque[5]   },   /* [未知] */
            { SG_HDR_WIDTH,       &h->width       },
            { SG_HDR_ZERO_A,      &h->zero_a      },
            { SG_HDR_ZERO_B,      &h->zero_b      },
            { 0x202038u,          &h->opaque[6]   },   /* [未知] */
            { 0x20203cu,          &h->opaque[7]   },   /* [未知] */
            { SG_HDR_LINES,       &h->lines       },
            { SG_HDR_PIXFMT,      &h->pixfmt      },
            { 0x202008u,          &h->opaque[3]   },   /* [未知] */
            { SG_HDR_SEQ,         &h->seq         },
            { SG_HDR_PAYLOAD_LEN, &h->payload_len },
            { SG_HDR_DEST_OFFSET, &h->dest_offset },
            { 0x202024u,          &h->opaque[2]   },   /* [未知] */
            { 0x202028u,          &h->opaque[0]   },   /* [未知] */
            { 0x20202cu,          &h->opaque[1]   },   /* [未知] */
            { SG_HDR_MAX_LINES,   &h->max_lines   },
            { SG_HDR_MAGIC,       &h->magic       },
        };

        for (i = 0; i < 20; ++i) {
            ret = sg_session_rd(s, steps[i].reg, steps[i].dst);
            if (ret) {
                return ret;
            }
        }
    }
    return 0;
}

/* ==== 检查一个包头（纯函数，不碰硬件）================================== */

int sg_block_header_check(const struct sg_block_header *h, uint32_t expect_seq,
                          uint64_t filled)
{
    uint32_t seq_number = h->seq & SG_SEQ_MASK;     /* 去掉"最后一包"那一位，剩包序号 */

    if (h->magic != SG_HDR_MAGIC_VALUE ||           /* 魔数 "SCBU" */
        h->block_id == 0 ||                         /* 块号不能为 0 */
        seq_number == 0 ||                          /* 包序号从 1 起 */
        h->width == 0 ||
        h->max_lines == 0 ||
        h->lines > h->max_lines ||                  /* 行数不能超过每块最大行数 */
        h->pixfmt != SG_PIXFMT_MONO8 ||             /* 只支持 Mono8 */
        h->zero_a != 0 || h->zero_b != 0 ||         /* 实测必须为 0 */
        h->payload_len == 0 ||
        seq_number != expect_seq ||                 /* 包序号必须接上 */
        h->dest_offset != filled) {                 /* 位置必须正好接在已收数据之后 */
        return -EPROTO;
    }
    return 0;
}

/* ==== 收一个包的数据（DMA）============================================= */

/* 把卡上 DDR 里 [source, source+length) 搬到 dst，每次最多 SG_DMA_CHUNK 字节。
 * 每次的超时 = min(单次 DMA 超时, 整块剩余时间)。 */
static int dma_payload(struct sg_capture_session *s, const struct sg_capture_options *o,
                       uint64_t deadline_ms, uint32_t source,
                       unsigned char *dst, uint32_t total)
{
    uint32_t done = 0;

    while (done < total) {
        uint32_t length = total - done;
        uint32_t timeout;
        uint64_t now, remaining;
        int ret;

        if (sg_session_cancelled(s)) {
            return -ECANCELED;
        }
        now = s->io.now_ms(s->io.context);
        if (now >= deadline_ms) {
            return -ETIMEDOUT;
        }
        remaining = deadline_ms - now;

        if (length > SG_DMA_CHUNK) {
            length = SG_DMA_CHUNK;
        }
        timeout = o->dma_timeout_ms;
        if (remaining < timeout) {
            timeout = (uint32_t)remaining;
        }

        ret = s->io.dma_read(s->io.context, source + done, dst + done, length, timeout);
        if (ret) {
            return ret;
        }
        done += length;
    }
    return 0;
}

/* ==== 收完整一个块 ===================================================== */

/* 实际收数的函数。出错时直接返回，释放内存由外面的 sg_block_receive 统一做。 */
static int receive_packets(struct sg_capture_session *s, const struct sg_capture_options *o,
                           struct sg_capture_frame *frame)
{
    struct sg_block_header h;
    uint64_t deadline = s->io.now_ms(s->io.context) + o->frame_timeout_ms;
    uint64_t capacity = 0;          /* 整块总字节数 = width × max_lines，第一个包定下来 */
    uint32_t block = 0, width = 0, max_lines = 0;   /* 第一个包定下来，后面必须一致 */
    uint32_t expect_seq = 1;
    size_t filled = 0;              /* 已经收到多少字节 */
    unsigned packets = 0;
    int completed = 0;

    while (!completed) {
        uint64_t end, source_end;
        int is_last;
        int ret;

        /* 读包头，检查包头本身。 */
        ret = sg_block_header_read(s, deadline, &h);
        if (ret) {
            return ret;
        }
        ret = sg_block_header_check(&h, expect_seq, filled);
        if (ret) {
            return ret;
        }

        end        = (uint64_t)h.dest_offset + h.payload_len;   /* 这包数据结束的位置 */
        source_end = (uint64_t)h.src_addr + h.payload_len;      /* 在卡 DDR 里的结束位置 */

        if (packets == 0) {
            /* 第一个包：定下整块容量。 */
            capacity = (uint64_t)h.width * h.max_lines;
            if (capacity > o->max_frame_bytes) {
                return -EFBIG;
            }
            block = h.block_id;
            width = h.width;
            max_lines = h.max_lines;
        } else if (block != h.block_id || width != h.width || max_lines != h.max_lines) {
            /* 后面的包：必须和第一个包属于同一块。 */
            return -EPROTO;
        }

        is_last = (h.seq & SG_SEQ_LAST) != 0;

        if (end > capacity ||                       /* 不能写出整块之外 */
            source_end > (UINT64_C(1) << 32) ||     /* 卡上地址只有 32 位 */
            (is_last && (h.lines == 0 || end != (uint64_t)width * h.lines))) {
            /* 最后一包：已收字节数必须正好 = 宽 × 行数 */
            return -EPROTO;
        }

        /* 第一个包时才分配内存，一次分够整块。 */
        if (frame->data == NULL) {
            frame->data = malloc((size_t)capacity);
            if (frame->data == NULL) {
                return -ENOMEM;
            }
        }

        /* 把这一包的数据搬进来。 */
        ret = dma_payload(s, o, deadline, h.src_addr, frame->data + filled, h.payload_len);
        if (ret) {
            return ret;
        }

        filled = (size_t)end;
        packets++;
        expect_seq++;
        completed = is_last;

        if (completed) {
            /* 最后再检查一次取消和总超时，然后才把结果填给调用者。 */
            if (sg_session_cancelled(s)) {
                return -ECANCELED;
            }
            if (s->io.now_ms(s->io.context) >= deadline) {
                return -ETIMEDOUT;
            }
            frame->length   = filled;
            frame->width    = width;
            frame->height   = h.lines;
            frame->block_id = block;
            frame->packets  = packets;
        }
    }
    return 0;
}

int sg_block_receive(struct sg_capture_session *s, const struct sg_capture_options *o,
                     struct sg_capture_frame *frame)
{
    int ret = receive_packets(s, o, frame);

    if (ret) {
        sg_capture_frame_free(frame);   /* 失败时不把半块数据留给调用者 */
    }
    return ret;
}
