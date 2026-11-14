/* SPDX-License-Identifier: MIT
 *
 * sg_session.h —— 一次和采集卡"对话"的上下文，以及超时参数。
 * capture/ 里其他文件都以 struct sg_capture_session * 为第一个参数。
 *
 * 状态：✅ 定义原样来自 legacy sg3000_capture_core.h 第 30、46-54 行
 */
#ifndef SG_SESSION_H
#define SG_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "sg_capture_io.h"
#include "sg_frame.h"

#define SG_CAPTURE_MAX_BYTES (256u * 1024u * 1024u)  /* 单块上限 256 MiB */

struct sg_capture_options {
    uint32_t control_timeout_ms;   /* 邮箱命令超时，默认 1000，上限 2000 */
    uint32_t frame_timeout_ms;     /* 等一个完整块的超时，默认 10000，上限 60000 */
    uint32_t dma_timeout_ms;       /* 单次 DMA 超时，默认 1000，上限 2000 */
    size_t max_frame_bytes;        /* 默认 SG_CAPTURE_MAX_BYTES */
};

struct sg_capture_session {
    struct sg_capture_io io;       /* 向下的回调表（拷贝一份）*/
    uint16_t request_id;           /* 邮箱请求计数，每发一条 +1 */
    uint16_t last_ack;             /* 上一条回复的 ACK 计数 */
    int have_ack;                  /* 是否已收到过回复（之后 ACK 必须连续）*/
    /* 诊断用：最近一条邮箱回复的前 64 字节（出错时打印出来对照），不参与任何判断 */
    unsigned char last_reply[64];
    uint32_t last_reply_len;       /* 回复的完整字节数 */
    uint32_t dma_chunk;            /* 每次 DMA 最多搬多少字节，init 时按 io.max_dma 定 */
    /* 吞吐统计（只累加，不参与判断）：DMA 花了多久、等卡出数据花了多久。
     * wait_ms 接近 0 而 dma_ms 很大 = DMA 跟不上相机；反过来 = 板子在等相机。 */
    struct {
        uint64_t dma_bytes, dma_calls, dma_ms, wait_ms;
    } stats;
    /* 诊断用：采集走到了哪一步、最近一个块头的关键字段。出错时打印，不参与任何判断。 */
    const char *stage;
    struct {
        int valid;
        uint32_t block_id, seq, width, lines, max_lines, payload_len, dest_offset, src_addr;
    } last_hdr;
};

/* 填默认超时。 */
void sg_capture_default_options(struct sg_capture_options *o);

/* 检查回调表齐全（read_reg/write_reg/dma_read/now_ms/sleep_ms 不能为空），
 * 清零会话并拷贝回调表。缺回调返回 -EINVAL。 */
int sg_capture_session_init(struct sg_capture_session *s, const struct sg_capture_io *io);

/* —— 供 capture/ 内部使用的小工具（迁移自 legacy core.c 第 18-23 行）—— */
int sg_session_cancelled(struct sg_capture_session *s);
int sg_session_rd(struct sg_capture_session *s, uint32_t offset, uint32_t *v);
int sg_session_wr(struct sg_capture_session *s, uint32_t offset, uint32_t v);

#endif
