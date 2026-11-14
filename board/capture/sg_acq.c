/* SPDX-License-Identifier: MIT
 *
 * sg_acq.c —— 开始/停止采集，连续取块，单帧便捷函数。
 * 职责：管"状态机"（有没有开始、有没有失败、停没停），具体收数交给 sg_block。
 * 依赖：sg_preflight、sg_mailbox、sg_block、sg3000_regs.h
 * 被谁用：board/apps/、board/stream/（经 sg_source 回调）
 *
 * 迁移来源：legacy sg3000_capture_core.c
 *   stop_capture()（本文件 static）← 第 136-148 行
 *   sg_stream_begin                ← 第 150-172 行
 *   sg_stream_end                  ← 第 174-181 行
 *   sg_stream_next                 ← 第 244-257 行
 *   sg_capture_once                ← 第 259-274 行
 * 怎么验证：
 *   1. 单帧：与 legacy 程序同条件采一帧，输出逐字节相同
 *   2. 连续：采 100 块，blocks = 100，看 id_discontinuities 是否为 0
 *   3. 中途 Ctrl+C：必须看到 STOP 被执行、SG_ACQ_STATE 变为 "STOP"
 *
 * 一次采集的生命周期：
 *
 *     sg_stream_begin    预检 → 确认没有残留的块 → 写 ARM → 邮箱写 START
 *     sg_stream_next     取一个块（可以反复调用）
 *     sg_stream_end      邮箱写 STOP → 等状态寄存器变成 "STOP"
 *
 * 只要 begin 里尝试过 START（哪怕失败），end 就一定会执行 STOP。
 */
#include <string.h>

#include "sg_acq.h"
#include "sg_block.h"
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

/* ==== 停止 ============================================================= */

/* 发 STOP 命令，然后等 SG_ACQ_STATE 变成 "STOP"。
 * 邮箱写用 cleanup = 1：用户按了 Ctrl+C 也要把 STOP 发完；但等待仍有超时。 */
static int stop_capture(struct sg_capture_session *s, uint32_t timeout_ms)
{
    uint32_t state;
    uint64_t deadline;
    int ret;

    s->stage = "AcquisitionStop mailbox";
    ret = sg_mbx_write(s, SG_L_ACQ_STOP, timeout_ms, 1);
    if (ret) {
        return ret;
    }

    s->stage = "wait SG_ACQ_STATE = STOP";
    deadline = s->io.now_ms(s->io.context) + timeout_ms;
    for (;;) {
        if (s->io.now_ms(s->io.context) >= deadline) {
            return -ETIMEDOUT;
        }
        ret = sg_session_rd(s, SG_ACQ_STATE, &state);
        if (ret) {
            return ret;
        }
        if (state == SG_ACQ_STATE_STOP) {
            return 0;
        }
        s->io.sleep_ms(s->io.context, 1);
    }
}

/* ==== 开始 ============================================================= */

/* 检查参数是否在允许范围内（每个超时都不能为 0，也不能超过上限）。 */
static int options_valid(const struct sg_capture_options *o)
{
    return o->control_timeout_ms != 0 && o->control_timeout_ms <= 2000 &&
           o->frame_timeout_ms   != 0 && o->frame_timeout_ms   <= 60000 &&
           o->dma_timeout_ms     != 0 && o->dma_timeout_ms     <= 2000 &&
           o->max_frame_bytes    != 0 && o->max_frame_bytes    <= SG_CAPTURE_MAX_BYTES;
}

int sg_stream_begin(struct sg_capture_stream *st, struct sg_capture_session *s,
                    const struct sg_capture_options *o)
{
    uint32_t ready;
    int ret;

    if (st == NULL) {
        return -EINVAL;
    }
    memset(st, 0, sizeof(*st));
    if (s == NULL || o == NULL || !options_valid(o)) {
        return -EINVAL;
    }

    st->session = s;
    st->options = *o;

    /* ① 预检：不通过就不开始。 */
    s->stage = "preflight";
    ret = sg_capture_preflight(s, o->control_timeout_ms, &st->preflight);
    if (ret) {
        return ret;
    }
    if (sg_session_cancelled(s)) {
        return -ECANCELED;
    }

    /* ② 开始前不应该已经有数据块就绪，否则说明有残留，不去清理，直接报错。 */
    s->stage = "check no stale block (SG_HDR_READY)";
    ret = sg_session_rd(s, SG_HDR_READY, &ready);
    if (ret) {
        return ret;
    }
    if (ready) {
        return -EBUSY;
    }

    /* ③ 从这里开始，只要走到了"尝试 START"，end 就必须做 STOP。
     *    所以先记下 start_attempted，哪怕下面的写失败了也要清理。 */
    st->start_attempted = 1;
    s->stage = "write SG_ACQ_ARM";
    ret = sg_session_wr(s, SG_ACQ_ARM, SG_ACQ_ARM_VALUE);
    if (ret) {
        return ret;
    }
    s->stage = "AcquisitionStart mailbox";
    ret = sg_mbx_write(s, SG_L_ACQ_START, o->control_timeout_ms, 0);
    if (!ret) {
        st->active = 1;
    }
    return ret;
}

/* ==== 结束 ============================================================= */

int sg_stream_end(struct sg_capture_stream *st)
{
    if (st == NULL) {
        return -EINVAL;
    }
    /* 没尝试过 START，或者已经 STOP 过了：什么都不做，只把上次的结果再返回一次。 */
    if (!st->start_attempted || st->stop_attempted) {
        return st->stop_error;
    }

    st->stop_attempted = 1;
    st->active = 0;
    st->stop_error = stop_capture(st->session, st->options.control_timeout_ms);
    return st->stop_error;
}

/* ==== 取一个块 ========================================================= */

int sg_stream_next(struct sg_capture_stream *st, struct sg_capture_frame *frame)
{
    int ret;

    if (st == NULL || frame == NULL || !st->active || st->failed) {
        return -EINVAL;
    }
    memset(frame, 0, sizeof(*frame));

    ret = sg_block_receive(st->session, &st->options, frame);
    if (ret) {
        st->failed = 1;     /* 失败后不再继续取，调用者必须调用 end */
        return ret;
    }

    /* 块号不连续只计数，不推断丢了多少行。第一个块没有"上一个"，不算不连续。 */
    st->last_discontinuity = st->blocks != 0 &&
                             frame->block_id != (uint32_t)(st->last_block_id + 1u);
    if (st->last_discontinuity) {
        st->id_discontinuities++;
    }
    st->last_block_id = frame->block_id;
    st->blocks++;
    return 0;
}

/* ==== 单帧便捷函数 ===================================================== */

int sg_capture_once(struct sg_capture_session *s, const struct sg_capture_options *o,
                    struct sg_capture_result *r)
{
    struct sg_capture_stream st;
    int ret;

    if (r == NULL) {
        return -EINVAL;
    }
    memset(r, 0, sizeof(*r));

    ret = sg_stream_begin(&st, s, o);
    r->preflight = st.preflight;            /* begin 失败时也要带回已读到的预检值 */
    if (!ret) {
        ret = sg_stream_next(&st, &r->frame);
    }
    r->primary_error = ret;
    r->primary_stage = ret ? s->stage : NULL;

    /* 不管前面成功与否，都要 STOP（end 自己会判断是否真的需要）。 */
    r->stop_error = sg_stream_end(&st);
    r->stop_stage = r->stop_error ? s->stage : NULL;
    r->start_attempted = st.start_attempted;
    r->stop_attempted  = st.stop_attempted;

    /* 任何一个出错，都不把半成品的图像交出去。 */
    if (ret || r->stop_error) {
        sg_capture_frame_free(&r->frame);
    }
    return ret ? ret : r->stop_error;
}
