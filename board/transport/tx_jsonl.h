/* SPDX-License-Identifier: MIT
 *
 * tx_jsonl.h —— 把处理结果写成一行 "SGJSON {...}" 到标准输出（经串口被电脑端中控台读走）。
 * 它是一个 sg_result_sink 实现。电脑端解析者：host/console/transport.py、telemetry.py。
 *
 * 规则（照搬 legacy）：结果为 UNKNOWN 时可按 interval_ns 抽样输出，
 * PASS/FAIL 一律不能丢。写 stdout 有超时，不会把流水线卡死。
 *
 * 状态：🟡 迁移来源 legacy sg_stream_main.c 第 25、63-101 行（emit、publish、struct output）
 * 将来：调试用，正式数据走 tx_tcp。
 */
#ifndef TX_JSONL_H
#define TX_JSONL_H
#include <stdint.h>
#include "sg_pipeline.h"

struct tx_jsonl {
    const char *source, *processor;   /* 写进每行的标签，如 "hardware" / "stats" */
    uint64_t last_ns, interval_ns, seen;
};

/* 填好一个 sg_result_sink，context 指向 t。 */
void tx_jsonl_sink(struct tx_jsonl *t, struct sg_result_sink *sink);

#endif
