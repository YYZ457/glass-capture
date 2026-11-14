/* SPDX-License-Identifier: MIT
 *
 * sg_capture_io.h —— 采集协议层"向下"的唯一接口：一张回调表。
 *
 * 为什么要有它：board/capture/ 里的协议代码只调用这张表，从不直接调用
 * ioctl。所以同一份协议代码可以：
 *   - 在板子上接真硬件（board/capture/sg_io_linux.c 实现这张表）
 *   - 在电脑上接假硬件做单元测试（verify/ 里可以写一个模拟实现）
 *
 * 约定（原样来自 legacy/sg3000-core-v0.2/sg3000_capture_core.h 第 33-45 行）：
 *   - 所有函数成功返回 0，失败返回负 errno（见 sg_err.h）
 *   - dma_read 只有成功时才保证恰好拷贝 length 字节
 *   - now_ms 必须单调递增；sleep_ms 必须真的等待
 *   - 回调负责硬件访问顺序和 DMA 缓冲区的生命周期
 * 状态：✅ 结构未改动，单帧采集实测可用
 */
#ifndef SG_CAPTURE_IO_H
#define SG_CAPTURE_IO_H
#include <stdint.h>

struct sg_capture_io {
    void *context;
    /* BAR0 寄存器读写，offset 见 sg3000_regs.h 的 A 类 */
    int (*read_reg)(void *ctx, uint32_t offset, uint32_t *value);
    int (*write_reg)(void *ctx, uint32_t offset, uint32_t value);
    /* 从卡上 DDR 的 source 地址搬 length 字节到 buffer，length <= max_dma */
    int (*dma_read)(void *ctx, uint32_t source, void *buffer, uint32_t length,
                    uint32_t timeout_ms);
    uint64_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    /* 可为 NULL；返回非 0 表示用户要求中止（Ctrl+C） */
    int (*cancelled)(void *ctx);
    /* 驱动每次最多搬多少字节（sg_info.max_transfer）。0 = 按 SG_DMA_CHUNK（64 KiB，所有驱动都支持）。
     * dma_fast=1 的驱动报 SG_DMA_MAX_CHUNK（1 MiB）。 */
    uint32_t max_dma;
};

#endif
