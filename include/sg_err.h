/* SPDX-License-Identifier: MIT
 *
 * sg_err.h —— 错误码约定。全项目统一：成功 0，失败返回负的标准 errno。
 * 不新造错误码，这里只写清楚"在本项目里它代表什么"，方便看日志对号入座。
 *
 *   -EINVAL    (-22)  参数不合法；或寄存器不在驱动白名单里
 *   -EPERM     (-1)   邮箱只允许写 ACQ_START / ACQ_STOP，写其他逻辑地址被拒
 *   -EIO       (-5)   硬件报错；或 DMA 停不下来，驱动已把卡隔离（poisoned）
 *   -ETIMEDOUT (-110) 规定时间内没等到硬件
 *   -ENOLINK   (-67)  预检不通过：DDR 状态不是 0/2，或 Camera Link 没时钟
 *   -ENOTSUP   (-95)  相机配置不受支持（只支持 Base、黑白、8 位）
 *   -EPROTO    (-71)  硬件回答和协议对不上（魔数、包序号、长度、ACK 计数）
 *   -EBUSY     (-16)  设备被占用；邮箱里有残留回复；或开始前已有块就绪
 *   -ENOSPC    (-28)  邮箱发送 FIFO 空间不够
 *   -EFBIG     (-27)  块大小超过 max_frame_bytes
 *   -ENOMEM    (-12)  内存不够
 *   -ECANCELED (-125) 用户中止（Ctrl+C）
 *   -ENOBUFS   (-105) 连续采集时队列满
 *   -ENOSYS    (-38)  ★ 骨架占位：这个函数还没填
 *
 * 2026-10-30 日志里的 "Link has been severed (-67)" 就是 -ENOLINK。
 */
#ifndef SG_ERR_H
#define SG_ERR_H
#include <errno.h>
#endif
