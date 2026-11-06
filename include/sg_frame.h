/* SPDX-License-Identifier: MIT
 *
 * sg_frame.h —— 一个"采集块"在各层之间传递时的样子。
 *
 * 注意：线阵相机没有物理意义上的"帧"。卡把连续若干行拼成一个块交给主机，
 * 这里的 frame 指的就是这个块。块边界 ≠ 玻璃上的任何物理边界。
 *
 * 谁产生：board/capture/sg_acq.c（sg_stream_next）
 * 谁消费：board/stream/、board/processing/、board/apps/
 * 所有权：data 由 malloc 分配；收到它的人负责调用 sg_capture_frame_free。
 *
 * 状态：✅ 结构原样来自 legacy sg3000_capture_core.h 第 60-64 行
 */
#ifndef SG_FRAME_H
#define SG_FRAME_H
#include <stddef.h>
#include <stdint.h>

struct sg_capture_frame {
    unsigned char *data;   /* Mono8，width * height 字节，行连续存放 */
    size_t length;         /* 实际字节数 */
    uint32_t width;        /* 卡报告的宽度（目前 4096，相机是 16384，原因待查）*/
    uint32_t height;       /* 本块行数 */
    uint32_t block_id;     /* 卡给的块编号；相邻块不连续时只计数，不推断丢了几行 */
    uint32_t packets;      /* 这个块分几包收到（实测 1 包）*/
};

/* 释放 data 并清零结构体；frame 为 NULL 时什么都不做。实现在 board/capture/sg_session.c */
void sg_capture_frame_free(struct sg_capture_frame *frame);

#endif
