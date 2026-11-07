/* SPDX-License-Identifier: MIT
 *
 * cam_serial.h —— Camera Link 串口通道（相机 ⇄ 采集卡 ⇄ 主机）。⬜❓ 全新
 *
 * Camera Link 线缆里有一对串口线，相机的所有配置命令都走这里，由采集卡转发。
 * 现有代码**完全没有**这条通道，所以相机一直跑出厂默认配置。
 *
 * 已知（2026-11-07）：相机讲 GenCP 协议（按地址读写寄存器），地址表见 cam_regs.h。
 * 还缺：采集卡在 Linux 下怎么收发串口字节 —— 见 docs/interfaces/I7 "未知"。
 *
 * 短期替代：在 Windows 上用 VisionKit 配好并保存到相机 User Set，本模块可延后。
 */
#ifndef CAM_SERIAL_H
#define CAM_SERIAL_H
#include <stddef.h>
#include <stdint.h>
#include "sg_session.h"

/* GenCP 读写一个 32 位寄存器（大端）。 */
int cam_gencp_read32(struct sg_capture_session *s, uint32_t addr, uint32_t *value, uint32_t timeout_ms);
int cam_gencp_write32(struct sg_capture_session *s, uint32_t addr, uint32_t value, uint32_t timeout_ms);

#endif
