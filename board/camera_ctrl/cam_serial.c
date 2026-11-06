/* SPDX-License-Identifier: MIT
 *
 * cam_serial.c —— ⬜❓ 等厂家资料。不要靠猜测寄存器来写：写错卡的配置寄存器可能让卡进入异常状态。
 */
#include "cam_serial.h"
#include "sg_err.h"

int cam_serial_transact(struct sg_capture_session *s, const char *cmd,
                        char *reply, size_t reply_cap, uint32_t timeout_ms)
{ (void)s; (void)cmd; (void)reply; (void)reply_cap; (void)timeout_ms; return -ENOSYS; }
