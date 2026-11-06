/* SPDX-License-Identifier: MIT
 *
 * sg_preflight.c —— 采集前体检的实现。
 * 职责：读状态、给判定，不改变卡的任何状态（只读，零风险）。
 * 依赖：sg_mailbox（sg_mbx_read）、sg3000_regs.h 的 SG_L_*
 * 被谁用：sg_acq.c（开始采集前）、apps/sg_status.c
 *
 * 迁移来源：legacy sg3000_capture_core.c 第 85-106 行（sg_capture_preflight）
 *   迁移时把 0x10540024 等裸地址换成 sg3000_regs.h 里的名字。
 * 怎么验证：
 *   1. 正常接线：返回 0，五项值与上面头文件里的实测值一致
 *   2. 拔掉 Camera Link 线：应返回 -ENOLINK
 */
#include "sg_preflight.h"
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

int sg_capture_preflight(struct sg_capture_session *s, uint32_t timeout_ms,
                         struct sg_capture_preflight *p)
{ (void)s; (void)timeout_ms; (void)p; return -ENOSYS; /* TODO */ }
