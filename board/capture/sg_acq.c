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
 */
#include "sg_acq.h"
#include "sg_block.h"
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

int sg_stream_begin(struct sg_capture_stream *st, struct sg_capture_session *s,
                    const struct sg_capture_options *o)
{ (void)st; (void)s; (void)o; return -ENOSYS; /* TODO */ }

int sg_stream_next(struct sg_capture_stream *st, struct sg_capture_frame *frame)
{ (void)st; (void)frame; return -ENOSYS; /* TODO */ }

int sg_stream_end(struct sg_capture_stream *st) { (void)st; return -ENOSYS; /* TODO */ }

int sg_capture_once(struct sg_capture_session *s, const struct sg_capture_options *o,
                    struct sg_capture_result *r)
{ (void)s; (void)o; (void)r; return -ENOSYS; /* TODO */ }
