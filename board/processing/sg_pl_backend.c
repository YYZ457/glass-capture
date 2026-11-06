/* SPDX-License-Identifier: MIT
 *
 * sg_pl_backend.c —— 把处理任务交给 PL（FPGA 逻辑）的处理器，经 include/sg_pl_uapi.h 的 ioctl。
 * 职责：用户态这一半。另一半（PL 内核驱动 + Vivado 工程）**目前不存在**。
 * 依赖：include/sg_pl_uapi.h（提议中的 ABI，没有任何驱动实现它）
 * 迁移来源：legacy sg_pl_linux.c（58 行）
 * 状态：🟡 无法运行。见 docs/decisions/D001_pl_not_in_datapath.md
 * 建议：PL 进入数据通路之前不要迁移这个文件。
 */
#include "sg_processing.h"
#include "sg_err.h"

int sg_pl_open(struct sg_processor *out, const char *device)
{ (void)out; (void)device; return -ENOSYS; /* 暂不实现 */ }
