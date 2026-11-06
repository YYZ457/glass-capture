/* SPDX-License-Identifier: MIT
 *
 * sg_io_linux.h —— sg_capture_io 回调表的 Linux 实现：把"读寄存器 / DMA"翻译成 ioctl。
 *
 * 这是 capture/ 里**唯一**认识 /dev/sg3000_rx_* 和 ioctl 的地方。
 * legacy 里这段代码在 sg3000_capture.c 和 sg_stream_main.c 各抄了一份，这里合并成一份。
 *
 * 状态：✅ 单帧程序里这套回调实测可用
 */
#ifndef SG_IO_LINUX_H
#define SG_IO_LINUX_H
#include <signal.h>
#include "sg_capture_io.h"
#include "sg3000_rx_uapi.h"

#define SG_DEFAULT_DEVICE "/dev/sg3000_rx_0000:01:00.0"  /* [观察] 实测路径 */
#define SG_CONTROL_LOCK   "/run/sg3000-control.lock"     /* 同一时间只允许一个程序控制卡 */

struct sg_linux_dev {
    int fd;                           /* 设备文件 */
    int lock_fd;                      /* 独占锁 */
    struct sg_info info;              /* SG_IOC_INFO 的结果 */
    volatile sig_atomic_t *cancel;    /* 指向调用者的 Ctrl+C 标志，可为 NULL */
};

/* 拿独占锁 → 打开设备 → SG_IOC_INFO → 检查 abi/vendor/device/poisoned。
 * 任何一步失败都返回负 errno，并已清理好。 */
int sg_linux_open(struct sg_linux_dev *d, const char *device, volatile sig_atomic_t *cancel);
void sg_linux_close(struct sg_linux_dev *d);

/* 填好一张回调表，context 指向 d。 */
void sg_linux_io(struct sg_linux_dev *d, struct sg_capture_io *io);

#endif
