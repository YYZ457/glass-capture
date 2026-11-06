/* SPDX-License-Identifier: MIT
 *
 * sg3000-capture —— 单帧采集程序。
 *
 * 用法（与 legacy 完全一致，中控台依赖这个命令行格式）：
 *   sg3000-capture preflight [--device PATH]
 *   sg3000-capture capture --output FILE.pgm [--device PATH] [--timeout-ms N]
 *
 * 组装：sg_io_linux（加锁、打开、INFO 检查）→ sg_session → sg_capture_once → pgm_write
 * 本文件只做"解析参数 + 组装 + 打印结果"，不含任何协议细节。
 *
 * 迁移来源：legacy sg3000_capture.c 第 113-192 行（usage、main）
 *   原来写在这里的 6 个回调已移到 board/capture/sg_io_linux.c
 *   原来的 save_pgm 已移到 pgm_write.c
 * 怎么验证：与 legacy 版本在同一条件下各采一帧，两个 PGM 文件逐字节相同。
 * 状态：✅（legacy 版本实测）
 *
 * ⚠️ 板上现在跑的二进制是旧版本，靠给文件偏移 5736 处打 NOP 补丁才能跳过 DDR 检查。
 *    用本仓库（或 legacy 源码）重新编译后就不再需要补丁，见 docs/decisions/D003。
 */
#include <stdio.h>
#include "sg_io_linux.h"
#include "sg_acq.h"
#include "pgm_write.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "sg3000-capture: skeleton only, use legacy build\n");
    return 2;
}
