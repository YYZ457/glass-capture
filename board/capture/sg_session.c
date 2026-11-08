/* SPDX-License-Identifier: MIT
 *
 * sg_session.c —— 会话初始化、默认参数、帧释放、读写小工具。
 * 职责：只管"会话"这个容器本身，不碰任何寄存器含义。
 * 依赖：sg_capture_io.h（回调表）
 * 被谁用：capture/ 下所有文件、apps/
 *
 * 迁移来源：legacy/sg3000-core-v0.2/sg3000_capture_core.c
 *   sg_session_cancelled / rd / wr      ← 第 18-23 行
 *   sg_capture_default_options          ← 第 25-29 行
 *   sg_capture_session_init             ← 第 30-35 行
 *   sg_capture_frame_free               ← 第 36-37 行
 * 怎么验证：电脑上用假回调表调用，缺任一必需回调应返回 -EINVAL。
 */
#include <stdlib.h>
#include <string.h>

#include "sg_session.h"
#include "sg_err.h"

/* ---- 三个小工具：把 s->io 回调表的调用包短一点 ---------------------- */

/* 用户是否要求中止（Ctrl+C）。cancelled 回调允许为空，为空就当作"没人中止"。 */
int sg_session_cancelled(struct sg_capture_session *s)
{
    if (s->io.cancelled == NULL) {
        return 0;
    }
    return s->io.cancelled(s->io.context) != 0;
}

/* 读 BAR0 寄存器。offset 是相对 BAR0 起点的偏移，读到的值通过 v 带回。 */
int sg_session_rd(struct sg_capture_session *s, uint32_t off, uint32_t *v)
{
    return s->io.read_reg(s->io.context, off, v);
}

/* 写 BAR0 寄存器。写的值 v 直接传进去。 */
int sg_session_wr(struct sg_capture_session *s, uint32_t off, uint32_t v)
{
    return s->io.write_reg(s->io.context, off, v);
}

/* ---- 默认参数 -------------------------------------------------------- */

void sg_capture_default_options(struct sg_capture_options *o)
{
    o->control_timeout_ms = 1000;               /* 邮箱命令 */
    o->frame_timeout_ms   = 10000;              /* 等一个完整块 */
    o->dma_timeout_ms     = 1000;               /* 单次 DMA */
    o->max_frame_bytes    = SG_CAPTURE_MAX_BYTES;
}

/* ---- 会话初始化 ------------------------------------------------------ */

int sg_capture_session_init(struct sg_capture_session *s, const struct sg_capture_io *io)
{
    /* cancelled 允许为空，所以不在检查之列。 */
    if (s == NULL || io == NULL ||
        io->read_reg == NULL || io->write_reg == NULL ||
        io->dma_read == NULL ||
        io->now_ms == NULL || io->sleep_ms == NULL) {
        return -EINVAL;
    }

    memset(s, 0, sizeof(*s));   /* request_id、last_ack、have_ack 都从 0 开始 */
    s->io = *io;                /* 拷贝整张回调表（不是保存指针） */
    return 0;
}

/* ---- 释放一个采集块 -------------------------------------------------- */

/* 释放 data 并把结构体清零，所以对同一个 frame 调用两次也安全。 */
void sg_capture_frame_free(struct sg_capture_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}
