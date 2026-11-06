/* SPDX-License-Identifier: MIT
 *
 * sg_status —— 只读体检程序。⬜ 全新，建议第一个写（零风险：不启动采集、不写任何控制寄存器）。
 *
 * 用法：sg_status [--device PATH]
 * 输出：
 *   1. 驱动信息（SG_IOC_INFO）：PCIe 位置、ABI、引擎 ID、对齐要求、poisoned
 *   2. 预检结果：DDR 状态、时钟寄存器（逐位打印）、CL 配置、彩色、位深
 *   3. SG_HDR_READY、SG_ACQ_STATE 的当前值
 * 退出码：0 一切正常；1 预检不通过；2 打不开设备
 *
 * 组装：sg_io_linux（打开设备）→ sg_session → sg_preflight
 * 没有 legacy 来源；可参考 legacy sg3000_capture.c 的 "preflight" 子命令。
 */
#include <stdio.h>
#include "sg_io_linux.h"
#include "sg_session.h"
#include "sg_preflight.h"
#include "sg3000_regs.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "sg_status: not implemented yet\n");
    return 2;
}
