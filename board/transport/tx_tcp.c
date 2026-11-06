/* SPDX-License-Identifier: MIT
 *
 * tx_tcp.c —— ⬜ 待写。建议这是连续采集之前的下一个优先项：它让每次调试从几分钟变成一秒。
 * 怎么验证：发一块已知图（比如 sample 里的 PGM），电脑端收到后 SHA256 一致。
 */
#include "tx_tcp.h"
#include "sg_err.h"

int tx_tcp_connect(struct tx_tcp *t, const char *host, uint16_t port, uint32_t timeout_ms)
{ (void)t; (void)host; (void)port; (void)timeout_ms; return -ENOSYS; }
void tx_tcp_close(struct tx_tcp *t) { (void)t; }
int tx_tcp_send_frame(struct tx_tcp *t, const struct sg_capture_frame *f, uint32_t timeout_ms)
{ (void)t; (void)f; (void)timeout_ms; return -ENOSYS; }
int tx_tcp_send_candidates(struct tx_tcp *t, uint32_t block_id, const struct sg_candidate *c,
                           uint32_t count, uint32_t timeout_ms)
{ (void)t; (void)block_id; (void)c; (void)count; (void)timeout_ms; return -ENOSYS; }
