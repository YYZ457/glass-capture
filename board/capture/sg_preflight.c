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
 *   3. EightyBit 只接一根线：第 2、3 路时钟没有 → -ENOLINK（2026-11-14 实板就是这样）
 */
#include <string.h>

#include "sg_preflight.h"
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

uint32_t sg_preflight_required_clocks(uint32_t cl_configuration)
{
    switch (cl_configuration) {
    case SG_CL_BASE:       return 0x1u;   /* 一根线、一颗芯片 */
    case SG_CL_EIGHTY_BIT: return 0x7u;   /* 两根线、三颗芯片（X 在第一根，Y、Z 在第二根）*/
    default:               return 0;      /* Medium / Full：本项目的相机不用，不支持 */
    }
}

uint32_t sg_preflight_missing_clocks(const struct sg_capture_preflight *p)
{
    return sg_preflight_required_clocks(p->cl_configuration) & ~p->pixel_clocks;
}

int sg_capture_preflight(struct sg_capture_session *s, uint32_t timeout_ms,
                         struct sg_capture_preflight *p)
{
    int ret;

    if (s == NULL || p == NULL || timeout_ms == 0 || timeout_ms > 2000) {
        return -EINVAL;
    }
    memset(p, 0, sizeof(*p));

    /* 每读成功一项，就在 valid_mask 里点亮对应的一位（bit0..bit4）。
     * 所以即使中途失败，调用者也知道前面哪几项是有效的。 */

    /* ① DDR 状态 */
    ret = sg_mbx_read(s, SG_L_DDR_STATUS, &p->ddr_status, timeout_ms, 0);
    if (ret) {
        return ret;
    }
    p->valid_mask |= 1u;

    /* ② Camera Link 像素时钟 */
    ret = sg_mbx_read(s, SG_L_PIXEL_CLOCKS, &p->pixel_clocks, timeout_ms, 0);
    if (ret) {
        return ret;
    }
    p->valid_mask |= 2u;

    /* ③ Camera Link 配置（卡 XML：0 Base / 1 Medium / 2 Full / 4 EightyBit） */
    ret = sg_mbx_read(s, SG_L_CL_CONFIG, &p->cl_configuration, timeout_ms, 0);
    if (ret) {
        return ret;
    }
    p->valid_mask |= 4u;

    /* ④ 相机颜色类型（0 = 黑白） */
    ret = sg_mbx_read(s, SG_L_CAMERA_COLOR, &p->camera_color, timeout_ms, 0);
    if (ret) {
        return ret;
    }
    p->valid_mask |= 8u;

    /* ⑤ 像素位深 */
    ret = sg_mbx_read(s, SG_L_CAMERA_DEPTH, &p->camera_depth, timeout_ms, 0);
    if (ret) {
        return ret;
    }
    p->valid_mask |= 16u;

    /* 五项都读完再判断（以前先判时钟就返回，后三项是 0，打印出来会误导）。
     * DDR 状态 0（未初始化）或 2（已校准）都算正常。 */
    if (p->ddr_status != 0 && p->ddr_status != 2) {
        return -ENOLINK;
    }
    /* 只支持 Base 或 EightyBit（10 Tap）、黑白、8 位。 */
    if (sg_preflight_required_clocks(p->cl_configuration) == 0 ||
        p->camera_color != 0 || p->camera_depth != 8u) {
        return -ENOTSUP;
    }
    /* 这种配置要用到的每一路像素时钟都必须锁定（卡 XML：bit0..2 = PixelClock1..3）。
     * EightyBit 要两根线：第 2、3 路时钟在第二根 Camera Link 线上。 */
    if (sg_preflight_missing_clocks(p) != 0) {
        return -ENOLINK;
    }
    return 0;
}
