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
 */
#include "sg_io_linux.h"
#include "sg_err.h"

int sg_linux_open(struct sg_linux_dev *d, const char *device, volatile sig_atomic_t *cancel)
{ (void)d; (void)device; (void)cancel; return -ENOSYS; /* TODO */ }

void sg_linux_close(struct sg_linux_dev *d) { (void)d; /* TODO */ }

void sg_linux_io(struct sg_linux_dev *d, struct sg_capture_io *io) { (void)d; (void)io; /* TODO */ }
