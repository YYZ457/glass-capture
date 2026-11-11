/* SPDX-License-Identifier: MIT
 *
 * tx_tcp.h —— 经 ZCU102 网口把数据发到电脑。协议见 docs/interfaces/I6_board_host.md
 *
 * 板子主动连电脑（电脑上先跑 host/receiver/rx_tcp.py）。
 *   tx_tcp_send_frame       调试：发整块原图（8 MB 一块，千兆网约 0.1 s；串口要约 7 分钟）
 *   tx_tcp_send_candidates  正式：只发候选颗粒（满速 252 MB/s 原图发不出去）
 *   tx_tcp_send_status      一行 JSON 状态
 * 所有发送都有超时；任何一次失败后连接就不可再用，应 close 后重连。
 */
#ifndef TX_TCP_H
#define TX_TCP_H
#include <stdint.h>
#include "sg_frame.h"
#include "sg_candidates.h"

#define TX_TCP_DEFAULT_PORT 8770

struct tx_tcp { int fd; uint64_t sequence; };

int tx_tcp_connect(struct tx_tcp *t, const char *host, uint16_t port, uint32_t timeout_ms);
void tx_tcp_close(struct tx_tcp *t);
int tx_tcp_send_frame(struct tx_tcp *t, const struct sg_capture_frame *f, uint32_t timeout_ms);
int tx_tcp_send_candidates(struct tx_tcp *t, uint32_t block_id, const struct sg_candidate *c,
                           uint32_t count, uint32_t timeout_ms);
int tx_tcp_send_status(struct tx_tcp *t, uint32_t block_id, const char *json, uint32_t timeout_ms);

#endif
