/* SPDX-License-Identifier: MIT
 *
 * sg3000-stream —— 连续采集 + 处理程序。
 *
 * 组装：
 *   数据源   sg_source  ← 硬件（sg_io_linux + sg_acq 的 begin/next/end）或 合成（--synthetic）
 *   处理器   sg_processor ← sg_stats（以后 sg_candidates）
 *   出口     sg_result_sink ← tx_jsonl（以后 tx_tcp）
 *   引擎     sg_pipeline_run
 *
 * 迁移来源：legacy sg_stream_main.c（203 行）
 *   hardware_next / hardware_stop  ← 第 48-51 行（包一层 sg_stream_next / sg_stream_end）
 *   synthetic 源                   ← 第 24、52-62 行（保留在本文件：它只用于测试）
 *   参数解析、main                  ← 第 102-203 行
 *   回调 reg_read 等                ← 已合并到 board/capture/sg_io_linux.c
 *   emit / publish                 ← 已移到 board/transport/tx_jsonl.c
 * 怎么验证：先 --synthetic 跑通，再接硬件跑 100 块。
 * 状态：🟡 legacy 版本从没在实机上运行过
 */
#include <stdio.h>
#include "sg_io_linux.h"
#include "sg_acq.h"
#include "sg_pipeline.h"
#include "tx_jsonl.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "sg3000-stream: skeleton only\n");
    return 2;
}
