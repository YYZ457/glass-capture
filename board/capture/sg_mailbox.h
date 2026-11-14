/* SPDX-License-Identifier: MIT
 *
 * sg_mailbox.h —— 经 BAR0 0x412000 的邮箱，请卡上固件代读/代写"逻辑地址"。
 *
 * 报文格式（全部 [观察]，详见 docs/interfaces/I2_sg3000_regmap.md）：
 *   请求：读 7 字 / 写 8 字
 *     [0] 0x000155aa  [1] 0  [2] 长度(读12/写16) | 请求号<<16  [3] 0
 *     [4] 0x80000000 读 / 0x80020000 写  [5] 逻辑地址  [6] 4  [7] 写入值（仅写）
 *   回复：读 32 字节 / 写 28 字节
 *     魔数 0x000055aa、偏移 10 处是 ACK 计数（会话内必须连续 +1）、
 *     读回复的值在偏移 28
 *
 * 安全约束（照搬 legacy，不要放宽）：
 *   - 写只允许 sg_mbx_write_allowed() 列出的地址和值，其他返回 -EPERM：
 *     开始 / 停止采集，以及 5 个卡参数（扫描类型、Tap、CL 配置、宽、高）。值域来自卡的 XML。
 *   - 发送前回复区非空 → -EBUSY，不做任何自动清理或重试
 *
 * 状态：✅ 预检、启动、停止都经它实测通过
 */
#ifndef SG_MAILBOX_H
#define SG_MAILBOX_H
#include <stdint.h>
#include "sg_session.h"

/* 读一个逻辑地址。cleanup 非 0 时忽略取消标志（停止采集时用），但仍受 timeout 限制。 */
int sg_mbx_read(struct sg_capture_session *s, uint32_t logical_addr, uint32_t *value,
                uint32_t timeout_ms, int cleanup);

/* 写一个逻辑地址（写入值固定为 1，照搬观察到的报文）。 */
int sg_mbx_write(struct sg_capture_session *s, uint32_t logical_addr,
                 uint32_t timeout_ms, int cleanup);

/* 写一个卡参数（必须在 sg_mbx_write_allowed 里，且不是开始/停止）。只在停止采集时调用。
 * 格式：[4] 0x80020000 [5] 地址 [6] 4 [7] 值 —— LusterGraber.dll WriteData（0x18000d235）[逆向]。 */
int sg_mbx_write_value(struct sg_capture_session *s, uint32_t logical_addr, uint32_t value,
                       uint32_t timeout_ms);

/* 写白名单（纯函数）：允许返回 1。 */
int sg_mbx_write_allowed(uint32_t logical_addr, uint32_t value);

/* 卡存储块读：一次读 len 字节（4 的倍数，4..SG_MBX_BLOCK_MAX），回复只做 Windows 那两项检查。
 * 只给 sg_cardxml 用（读 URL 和 XML）；寄存器请用 sg_mbx_read。 */
#define SG_MBX_BLOCK_MAX 0x100u
int sg_mbx_read_block(struct sg_capture_session *s, uint32_t logical_addr, void *buf,
                      uint32_t len, uint32_t timeout_ms);

#endif
