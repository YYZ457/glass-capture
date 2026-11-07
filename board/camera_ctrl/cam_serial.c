/* SPDX-License-Identifier: MIT
 *
 * cam_serial.c —— ⬜❓ 协议已知（GenCP），缺 Linux 下经卡收发串口的方法。不要靠猜测寄存器来写：写错卡的配置寄存器可能让卡进入异常状态。
 */
#include "cam_serial.h"
#include "sg_err.h"

int cam_gencp_read32(struct sg_capture_session *s, uint32_t addr, uint32_t *value, uint32_t timeout_ms)
{ (void)s; (void)addr; (void)value; (void)timeout_ms; return -ENOSYS; }
int cam_gencp_write32(struct sg_capture_session *s, uint32_t addr, uint32_t value, uint32_t timeout_ms)
{ (void)s; (void)addr; (void)value; (void)timeout_ms; return -ENOSYS; }
