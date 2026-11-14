/* SPDX-License-Identifier: MIT
 *
 * cam_serial.c —— Camera Link 串口邮箱的收发。报文格式来自反汇编 XDMA.sys（[逆向]），
 * 见 docs/interfaces/I7_camera_serial.md。
 *
 * 当前板上的旧驱动白名单不包含 0x4110xx / 0x4130xx，所以 send/recv 在板上会得到 -EINVAL，
 * 不会碰到硬件。放开白名单的条件见 I7 第 4 节。
 */
#include <string.h>

#include "cam_serial.h"
#include "sg3000_regs.h"
#include "sg_err.h"

int cam_serial_init(struct cam_serial *c, struct sg_capture_session *s, unsigned port)
{
    if (!c || !s || port >= SG_SER_PORTS) return -EINVAL;
    c->s = s;
    c->port = port;
    c->seq = 0;
    return 0;
}

int cam_uart_divisor(uint32_t baud, uint32_t *divisor)
{
    static const uint32_t allowed[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    size_t i;

    if (!divisor) return -EINVAL;
    for (i = 0; i < sizeof allowed / sizeof allowed[0]; ++i) {
        if (allowed[i] == baud) {
            *divisor = (SG_UART_CLOCK_HZ + baud * 16u / 2u) / (baud * 16u);
            return 0;
        }
    }
    return -EINVAL;
}

int cam_serial_pack(unsigned port, uint16_t seq, const void *payload, uint32_t len,
                    uint32_t *words, size_t capacity)
{
    const unsigned char *p = payload;
    size_t n, i;

    if (port >= SG_SER_PORTS || !words || !payload || !len || len > CAM_SERIAL_MAX_PAYLOAD)
        return -EINVAL;
    n = 4u + (len + 3u) / 4u;
    if (capacity < n) return -ENOSPC;

    words[0] = SG_MBX_REQ_MAGIC;
    words[1] = SG_SER_WORD1(port);
    words[2] = len | ((uint32_t)seq << 16);
    words[3] = 0;
    memset(words + 4, 0, (n - 4u) * sizeof *words);
    for (i = 0; i < len; ++i)
        words[4 + i / 4] |= (uint32_t)p[i] << (8u * (i % 4u));   /* 小端 */
    return (int)n;
}

int cam_serial_payload_len(const uint32_t header[4], uint32_t total, uint32_t *len)
{
    uint32_t n;

    if (!header || !len || total < CAM_SERIAL_HDR_BYTES) return -EPROTO;
    n = (header[0] & 0xffffu) == 0x55aau ? (header[2] & 0xffffu) : total - CAM_SERIAL_HDR_BYTES;
    if (n > total - CAM_SERIAL_HDR_BYTES) return -EPROTO;
    *len = n;
    return 0;
}

int cam_serial_send(struct cam_serial *c, const void *payload, uint32_t len)
{
    uint32_t words[CAM_SERIAL_MAX_WORDS];
    int n, i, ret;

    if (!c || !c->s) return -EINVAL;
    n = cam_serial_pack(c->port, (uint16_t)(c->seq + 1u), payload, len, words, CAM_SERIAL_MAX_WORDS);
    if (n < 0) return n;
    c->seq++;
    for (i = 0; i < n; ++i) {
        ret = sg_session_wr(c->s, SG_SER_TX_DATA(c->port), words[i]);
        if (ret) return ret;
    }
    return sg_session_wr(c->s, SG_SER_TX_LEN(c->port), len + CAM_SERIAL_HDR_BYTES);
}

int cam_serial_recv(struct cam_serial *c, void *buf, uint32_t capacity, uint32_t *len,
                    uint32_t timeout_ms)
{
    struct sg_capture_session *s;
    uint32_t occupancy, total, header[4], payload, word, i, nwords;
    unsigned char *out = buf;
    uint64_t deadline;
    int ret;

    if (!c || !c->s || !len || (!buf && capacity)) return -EINVAL;
    s = c->s;
    deadline = s->io.now_ms(s->io.context) + timeout_ms;

    for (;;) {
        if (sg_session_cancelled(s)) return -ECANCELED;
        ret = sg_session_rd(s, SG_SER_RX_OCCUPANCY(c->port), &occupancy);
        if (ret) return ret;
        if (occupancy) break;
        if (s->io.now_ms(s->io.context) >= deadline) return -ETIMEDOUT;
        s->io.sleep_ms(s->io.context, 1);
    }

    ret = sg_session_rd(s, SG_SER_RX_LEN(c->port), &total);
    if (ret) return ret;
    if (total < CAM_SERIAL_HDR_BYTES || total > CAM_SERIAL_HDR_BYTES + 0xffffu) return -EPROTO;

    for (i = 0; i < 4; ++i) {
        ret = sg_session_rd(s, SG_SER_RX_DATA(c->port), &header[i]);
        if (ret) return ret;
    }
    ret = cam_serial_payload_len(header, total, &payload);

    /* 头之后的字全部读走，哪怕要报错，免得下一包错位。 */
    nwords = (total - CAM_SERIAL_HDR_BYTES + 3u) / 4u;
    for (i = 0; i < nwords; ++i) {
        uint32_t b;
        int r = sg_session_rd(s, SG_SER_RX_DATA(c->port), &word);
        if (r) return r;
        for (b = 0; b < 4; ++b) {
            uint32_t at = i * 4u + b;
            if (!ret && at < payload && at < capacity) out[at] = (unsigned char)(word >> (8u * b));
        }
    }
    if (ret) return ret;
    *len = payload;
    return payload > capacity ? -EMSGSIZE : 0;
}

int cam_serial_set_baud(struct cam_serial *c, uint32_t baud)
{
    /* 步骤已知（I7 第 3 节），但分频寄存器 0x1000..0x100c 在哪个 BAR 没确认：
     * BAR1 的 0x1000 是 XDMA C2H 引擎，写错会弄乱 DMA。确认之前不实现。 */
    (void)c; (void)baud;
    return -ENOSYS;
}

int cam_gencp_read32(struct sg_capture_session *s, uint32_t addr, uint32_t *value, uint32_t timeout_ms)
{
    /* 缺：GenCP 串口帧前缀里两个 CRC16 的算法（EMVA GenCP 标准或 VisionKitApi.dll 反汇编）。 */
    (void)s; (void)addr; (void)value; (void)timeout_ms;
    return -ENOSYS;
}

int cam_gencp_write32(struct sg_capture_session *s, uint32_t addr, uint32_t value, uint32_t timeout_ms)
{
    (void)s; (void)addr; (void)value; (void)timeout_ms;
    return -ENOSYS;
}
