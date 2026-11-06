/* SPDX-License-Identifier: MIT
 *
 * cam_serial.h —— Camera Link 串口通道（相机 ⇄ 采集卡 ⇄ 主机）。⬜❓ 全新
 *
 * Camera Link 线缆里有一对串口线，相机的所有配置命令都走这里，由采集卡转发。
 * 现有代码**完全没有**这条通道，所以相机一直跑出厂默认配置。
 *
 * 缺的信息（写之前必须先拿到）：
 *   1. 采集卡怎么转发串口：哪个寄存器 / 邮箱逻辑地址 / 波特率 —— 问厂家，或在 Windows 上观察 VisionKit
 *   2. 相机命令格式和命令表 —— 相机手册（docs/interfaces/I7_camera_serial.md）
 *
 * 短期替代：在 Windows 上用 VisionKit 配好并保存到相机 User Set，本模块可延后。
 */
#ifndef CAM_SERIAL_H
#define CAM_SERIAL_H
#include <stddef.h>
#include <stdint.h>
#include "sg_session.h"

/* 发送一条命令字符串，等待以 terminator 结尾的应答，写入 reply（以 '\0' 结尾）。 */
int cam_serial_transact(struct sg_capture_session *s, const char *cmd,
                        char *reply, size_t reply_cap, uint32_t timeout_ms);

#endif
