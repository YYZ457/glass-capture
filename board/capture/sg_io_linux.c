/* SPDX-License-Identifier: MIT
 *
 * sg_io_linux.c —— ioctl 后端。
 * 职责：打开/锁定设备，并实现 6 个回调：read_reg、write_reg、dma_read、now_ms、sleep_ms、cancelled。
 * 依赖：include/sg3000_rx_uapi.h（驱动约定）
 * 被谁用：board/apps/ 的所有程序
 *
 * 迁移来源：
 *   6 个回调            ← legacy sg3000_capture.c 第 29-62 行（sg_stream_main.c 第 26-47 行是同一份）
 *   打开、加锁、INFO 检查 ← legacy sg3000_capture.c main() 中 open / flock / SG_IOC_INFO 那几行
 * 注意：dma_read 成功的判定是 completed == 1 且 status 无错误位（0x00f80638 | 1），照搬。
 * 怎么验证：sg_status 程序能打印出 SG_IOC_INFO 的内容，poisoned = 0。
 *
 * 协议层（sg_mailbox.c 等）只认识 struct sg_capture_io 这张回调表，
 * 这个文件负责把表里每一项接到真正的系统调用上：
 *
 *     read_reg   → ioctl(fd, SG_IOC_READ_REG,  ...)
 *     write_reg  → ioctl(fd, SG_IOC_WRITE_REG, ...)
 *     dma_read   → ioctl(fd, SG_IOC_DMA_READ,  ...)
 *     now_ms     → clock_gettime
 *     sleep_ms   → nanosleep
 *     cancelled  → 看调用者给的 Ctrl+C 标志
 *
 * 回调表里的 context 指向 struct sg_linux_dev，每个回调第一步先把
 * void *context 转回 struct sg_linux_dev * 才能用。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "sg_io_linux.h"
#include "sg3000_regs.h"
#include "sg_err.h"

/* DMA 状态字里这些位表示出错（照搬 legacy）。 */
#define DMA_STATUS_ERROR_BITS   (0x00f80638u | 1u)

/* SG_IOC_INFO 里 engine_id 的检查（照搬 legacy）：高 16 位必须是 0x1fc1，
 * 低 16 位中 0x8f00 这几位必须为 0。这几位的具体含义未查证。 */
#define ENGINE_ID_HIGH          0x1fc1u
#define ENGINE_ID_LOW_MASK      0x8f00u

/* ==== 6 个回调 ========================================================= */

static int linux_read_reg(void *context, uint32_t offset, uint32_t *value)
{
    struct sg_linux_dev *d = context;
    struct sg_reg reg = { offset, 0 };

    if (ioctl(d->fd, SG_IOC_READ_REG, &reg) < 0) {
        return -errno;
    }
    *value = reg.value;
    return 0;
}

static int linux_write_reg(void *context, uint32_t offset, uint32_t value)
{
    struct sg_linux_dev *d = context;
    struct sg_reg reg = { offset, value };

    if (ioctl(d->fd, SG_IOC_WRITE_REG, &reg) < 0) {
        return -errno;
    }
    return 0;
}

static int linux_dma_read(void *context, uint32_t source, void *buffer,
                          uint32_t length, uint32_t timeout_ms)
{
    struct sg_linux_dev *d = context;
    struct sg_dma dma;

    memset(&dma, 0, sizeof(dma));
    dma.source     = source;
    dma.buffer     = (uint64_t)(uintptr_t)buffer;   /* 指针当成数字交给驱动 */
    dma.length     = length;
    dma.timeout_ms = timeout_ms;

    if (ioctl(d->fd, SG_IOC_DMA_READ, &dma) < 0) {
        return -errno;
    }
    if (dma.completed != 1 || (dma.status & DMA_STATUS_ERROR_BITS)) {
        return -EIO;
    }
    return 0;
}

static uint64_t linux_now_ms(void *context)
{
    struct timespec t;

    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        abort();    /* 单调时钟都取不到，后面的超时判断全部不可信，不如直接终止 */
    }
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void linux_sleep_ms(void *context, uint32_t ms)
{
    struct timespec pause;

    (void)context;
    pause.tv_sec  = ms / 1000u;
    pause.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&pause, NULL);      /* 被信号打断就提前醒来，调用者会再检查取消标志 */
}

static int linux_cancelled(void *context)
{
    struct sg_linux_dev *d = context;

    return d->cancel != NULL && *d->cancel != 0;
}

/* ==== 打开 / 关闭 ====================================================== */

int sg_linux_open(struct sg_linux_dev *d, const char *device, volatile sig_atomic_t *cancel)
{
    struct sg_info *info;
    int ret;

    if (d == NULL || device == NULL) {
        return -EINVAL;
    }
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->lock_fd = -1;
    d->cancel = cancel;
    info = &d->info;

    /* ① 独占锁：同一时间只允许一个程序控制这块卡。拿不到就立刻放弃，不等待。 */
    d->lock_fd = open(SG_CONTROL_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (d->lock_fd < 0) {
        ret = -errno;
        goto fail;
    }
    if (flock(d->lock_fd, LOCK_EX | LOCK_NB)) {
        ret = -EBUSY;
        goto fail;
    }

    /* ② 打开设备文件（驱动创建的）。 */
    d->fd = open(device, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        ret = -errno;
        goto fail;
    }

    /* ③ 向驱动问设备信息，核对是不是我们要的那块卡、驱动状态是否正常。 */
    if (ioctl(d->fd, SG_IOC_INFO, info) < 0) {
        ret = -errno;
        goto fail;
    }
    if (info->abi != SG_ABI_VERSION ||
        info->vendor != SG_PCI_VENDOR ||
        info->device != SG_PCI_DEVICE ||
        info->poisoned ||                           /* 驱动已把卡隔离，不能再用 */
        info->max_transfer < SG_DMA_CHUNK ||       /* 每个驱动至少支持 64 KiB */
        (info->engine_id >> 16) != ENGINE_ID_HIGH ||
        (info->engine_id & ENGINE_ID_LOW_MASK)) {
        ret = -ENODEV;
        goto fail;
    }
    return 0;

fail:
    sg_linux_close(d);      /* 任何一步失败，已经打开的都要关掉 */
    return ret;
}

void sg_linux_close(struct sg_linux_dev *d)
{
    if (d == NULL) {
        return;
    }
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    if (d->lock_fd >= 0) {
        close(d->lock_fd);      /* 关掉文件，锁也就自动释放了 */
        d->lock_fd = -1;
    }
}

/* ==== 填回调表 ========================================================= */

void sg_linux_io(struct sg_linux_dev *d, struct sg_capture_io *io)
{
    io->context   = d;
    io->read_reg  = linux_read_reg;
    io->write_reg = linux_write_reg;
    io->dma_read  = linux_dma_read;
    io->now_ms    = linux_now_ms;
    io->sleep_ms  = linux_sleep_ms;
    io->cancelled = linux_cancelled;
    /* 驱动说一次能搬多少就搬多少（dma_fast=1 是 1 MiB），但不超过本程序认识的上限 */
    io->max_dma   = d->info.max_transfer < SG_DMA_MAX_CHUNK ? d->info.max_transfer : SG_DMA_MAX_CHUNK;
}
