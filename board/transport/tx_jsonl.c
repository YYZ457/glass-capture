/* SPDX-License-Identifier: MIT
 *
 * tx_jsonl.c —— 迁移来源：legacy sg_stream_main.c 第 63-101 行（emit + publish）。
 * 怎么验证：合成源跑一段，用 host/tests/test_console.py 解析输出不报错。
 */
#include "tx_jsonl.h"
#include "sg_err.h"

void tx_jsonl_sink(struct tx_jsonl *t, struct sg_result_sink *sink) { (void)t; (void)sink; /* TODO */ }
