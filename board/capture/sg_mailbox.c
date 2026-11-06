/* SPDX-License-Identifier: MIT
 *
 * sg_mailbox.c —— 邮箱报文的组装、发送、等待、解析。
 * 职责：只管"把一条命令送到卡固件并拿回答复"，不知道任何逻辑地址的含义。
 * 依赖：sg_session（rd/wr/now/sleep）、sg3000_regs.h 的 SG_MBX_*
 * 被谁用：sg_preflight.c、sg_acq.c
 *
 * 迁移来源：legacy sg3000_capture_core.c
 *   le16 / le32 / put32                 ← 第 13-17 行（字节序小工具，可放本文件 static）
 *   logical()                           ← 第 39-83 行，拆成下面两个对外函数
 * 怎么验证：
 *   1. 电脑上用假回调表喂一段录下来的回复，检查解析出的值
 *   2. 板上连读两次 SG_L_DDR_STATUS，都应是 2，且 ACK 计数连续
 */
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

int sg_mbx_read(struct sg_capture_session *s, uint32_t logical_addr, uint32_t *value,
                uint32_t timeout_ms, int cleanup)
{ (void)s; (void)logical_addr; (void)value; (void)timeout_ms; (void)cleanup;
  return -ENOSYS; /* TODO */ }

int sg_mbx_write(struct sg_capture_session *s, uint32_t logical_addr,
                 uint32_t timeout_ms, int cleanup)
{ (void)s; (void)logical_addr; (void)timeout_ms; (void)cleanup;
  return -ENOSYS; /* TODO */ }
