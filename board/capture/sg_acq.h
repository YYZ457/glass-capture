/* SPDX-License-Identifier: MIT
 *
 * sg_acq.h —— 采集的生命周期：开始 → 一块一块取 → 停止。capture/ 对外的主入口。
 *
 * 规则（照搬 legacy core.h 第 71-75 行的约定）：
 *   - begin 只要尝试过 START（哪怕失败），end 就一定会做有超时的 STOP
 *   - next 每返回一个块，所有权就交给调用者
 *   - 块号不连续只计数（id_discontinuities），不推断丢了多少行
 *   - next 失败后必须调用 end，不做自动重新同步
 *
 * 开始的硬件顺序：预检 → 确认 SG_HDR_READY 为 0 → 写 SG_ACQ_ARM=2 → 邮箱写 START
 * 停止的硬件顺序：邮箱写 STOP（cleanup 模式）→ 轮询 SG_ACQ_STATE == "STOP"
 *
 * 状态：✅ 单帧（sg_capture_once）实测；🟡 连续 begin/next/end 从没在板上跑过
 */
#ifndef SG_ACQ_H
#define SG_ACQ_H
#include <stdint.h>
#include "sg_session.h"
#include "sg_preflight.h"

struct sg_capture_stream {
    struct sg_capture_session *session;
    struct sg_capture_options options;
    struct sg_capture_preflight preflight;
    uint64_t blocks, id_discontinuities;
    uint32_t last_block_id;
    int active, failed, start_attempted, stop_attempted, stop_error, last_discontinuity;
};

int sg_stream_begin(struct sg_capture_stream *st, struct sg_capture_session *s,
                    const struct sg_capture_options *o);
int sg_stream_next(struct sg_capture_stream *st, struct sg_capture_frame *frame);
int sg_stream_end(struct sg_capture_stream *st);

/* 单帧便捷函数 = begin + next + end，结果和两个错误码都放进 r */
struct sg_capture_result {
    struct sg_capture_preflight preflight;
    struct sg_capture_frame frame;
    int primary_error, stop_error;
    int start_attempted, stop_attempted;
    const char *primary_stage;      /* 诊断：primary_error 出在哪一步（成功时为 NULL）*/
    const char *stop_stage;         /* 诊断：stop_error 出在哪一步 */
};
int sg_capture_once(struct sg_capture_session *s, const struct sg_capture_options *o,
                    struct sg_capture_result *r);

#endif
