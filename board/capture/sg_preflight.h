/* SPDX-License-Identifier: MIT
 *
 * sg_preflight.h —— 采集前体检：卡和相机链路是不是处在能采集的状态。
 *
 * 依次经邮箱读 5 个逻辑地址（见 sg3000_regs.h 的 C 类），任何一项读失败就停。
 * 判定规则（照搬 legacy）：
 *   DDR 状态 ∉ {0, 2}，或时钟寄存器 bit0 = 0   → -ENOLINK
 *   CL 配置 ≠ 0（非 Base）、彩色、位深 ≠ 8       → -ENOTSUP
 * 实测值（2026-10-30）：DDR=2，时钟=0x59，CL=0，彩色=0，位深=8
 *
 * 状态：✅ 判定逻辑实测通过；时钟寄存器 0x59 中 bit3/4/6 的含义未知
 */
#ifndef SG_PREFLIGHT_H
#define SG_PREFLIGHT_H
#include <stdint.h>
#include "sg_session.h"

struct sg_capture_preflight {
    uint32_t ddr_status, pixel_clocks;
    uint32_t cl_configuration, camera_color, camera_depth;
    uint32_t valid_mask; /* bit0..4 依次表示上面 5 项是否读成功 */
};

/* timeout_ms 必须在 1..2000 之间。即使返回错误，p 里已读到的项也有效（看 valid_mask）。 */
int sg_capture_preflight(struct sg_capture_session *s, uint32_t timeout_ms,
                         struct sg_capture_preflight *p);

#endif
