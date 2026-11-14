/* SPDX-License-Identifier: MIT
 *
 * cam_serial.c —— Camera Link 串口邮箱 + GenCP。每一步都照 Windows 二进制的反汇编，
 * 出处地址见 docs/vendor/reverse_engineering_2026-11-14.md 第 2 部分。
 */
#include <string.h>

#include "cam_serial.h"
#include "sg3000_regs.h"
#include "sg_err.h"

/* ==== 第 1 层：串口邮箱（XDMA.sys）=================================== */

int cam_serial_init(struct cam_serial *c, struct sg_capture_session *s, unsigned port)
{
    if (!c || !s || port >= SG_SER_PORTS) return -EINVAL;
    memset(c, 0, sizeof *c);
    c->s = s;
    c->port = port;
    return 0;
}

/* XDMA.sys EvtDeviceFileCreate 0x14000145a..0x140001568 */
int cam_serial_open(struct cam_serial *c, struct sg_capture_session *s, unsigned port)
{
    uint32_t v;
    int tries, ret;

    ret = cam_serial_init(c, s, port);
    if (ret) return ret;
    ret = sg_session_rd(s, SG_SER_CTRL(port), &v);
    if (ret || v == SG_SER_CTRL_READY) return ret;

    for (tries = 3; tries > 0; --tries) {
        if ((ret = sg_session_wr(s, SG_SER_FLUSH_C(port), SG_SER_FLUSH_VALUE)) ||
            (ret = sg_session_wr(s, SG_SER_FLUSH_B(port), SG_SER_FLUSH_VALUE)) ||
            (ret = sg_session_wr(s, SG_SER_FLUSH_A(port), SG_SER_FLUSH_VALUE)) ||
            (ret = sg_session_rd(s, SG_SER_RX_OCCUPANCY(port), &v)))
            return ret;
        if (v == 0) break;
    }
    return sg_session_wr(s, SG_SER_CTRL(port), SG_SER_CTRL_READY);
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

/* XDMA.sys 0x140003030..0x140003265。写 DLL=0xff 那一步照抄，原因未知。 */
int cam_serial_set_baud(struct cam_serial *c, uint32_t baud)
{
    struct sg_capture_session *s;
    uint32_t div, lcr, ctrl;
    unsigned p;
    int ret;

    if (!c || !c->s) return -EINVAL;
    ret = cam_uart_divisor(baud, &div);
    if (ret) return ret;
    s = c->s;
    p = c->port;

    if ((ret = sg_session_rd(s, SG_UART_LCR(p), &lcr)) ||
        (ret = sg_session_wr(s, SG_UART_LCR(p), lcr | SG_UART_LCR_DLAB)) ||
        (ret = sg_session_wr(s, SG_UART_DLL(p), 0xffu)) ||
        (ret = sg_session_wr(s, SG_UART_DLM(p), (div >> 8) & 0xffu)) ||
        (ret = sg_session_wr(s, SG_UART_DLL(p), div & 0xffu)) ||
        (ret = sg_session_wr(s, SG_UART_LCR(p), lcr)) ||
        (ret = sg_session_rd(s, SG_SER_CTRL(p), &ctrl)))
        return ret;
    if (ctrl & SG_SER_CTRL_READY) return 0;
    return sg_session_wr(s, SG_SER_CTRL(p), SG_SER_CTRL_READY);
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
        words[4 + i / 4] |= (uint32_t)p[i] << (8u * (i % 4u));   /* 小端，尾部补 0 */
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

/* XDMA.sys 0x140003ae0..0x140003cbd */
int cam_serial_send(struct cam_serial *c, const void *payload, uint32_t len)
{
    uint32_t words[CAM_SERIAL_MAX_WORDS];
    int n, i, ret;

    if (!c || !c->s) return -EINVAL;
    n = cam_serial_pack(c->port, c->seq, payload, len, words, CAM_SERIAL_MAX_WORDS);
    if (n < 0) return n;
    c->seq++;
    for (i = 0; i < n; ++i) {
        ret = sg_session_wr(c->s, SG_SER_TX_DATA(c->port), words[i]);
        if (ret) return ret;
    }
    return sg_session_wr(c->s, SG_SER_TX_LEN(c->port), len + CAM_SERIAL_HDR_BYTES);
}

/* XDMA.sys 0x140003540..0x1400037c5（Windows 等中断，这里轮询）*/
int cam_serial_recv(struct cam_serial *c, void *buf, uint32_t capacity, uint32_t *len,
                    uint32_t timeout_ms)
{
    struct sg_capture_session *s;
    uint32_t occupancy, total, header[4], payload, word, i, b;
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
    if (ret) return ret;

    for (i = 0; i < (payload + 3u) / 4u; ++i) {
        ret = sg_session_rd(s, SG_SER_RX_DATA(c->port), &word);
        if (ret) return ret;
        for (b = 0; b < 4; ++b) {
            uint32_t at = i * 4u + b;
            if (at < payload && at < capacity) out[at] = (unsigned char)(word >> (8u * b));
        }
    }
    *len = payload;
    return payload > capacity ? -EMSGSIZE : 0;
}

/* ==== 第 2 层：GenCP（VisionKitApi.dll gcp2::GenCpCmdHelper）============
 *
 * 包（全部大端）：
 *   前缀 0  preamble 0x0100 | 2 CCD 校验和 | 4 SCD 校验和 | 6 channel 0
 *   CCD  8  flags / 状态码 | 10 命令号 | 12 SCD 长度 | 14 请求号
 *   SCD 16  ...
 * CCD 校验和覆盖字节 6..15；SCD 校验和覆盖字节 6..末尾。 */

#define GENCP_READ_CMD      0x0800u
#define GENCP_READ_ACK      0x0801u
#define GENCP_WRITE_CMD     0x0802u
#define GENCP_WRITE_ACK     0x0803u
#define GENCP_PENDING_ACK   0x0805u
#define GENCP_READ_FLAGS    0x4001u   /* VisionKitApi.dll 0x180029fc6：读命令用 0x4001 */
#define GENCP_WRITE_FLAGS   0x4000u   /* 0x18002a1c8：写命令用 0x4000 */

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put64(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (56 - 8 * i));
}

/* VisionKitApi.dll 0x18003fac0 */
uint16_t cam_gencp_checksum(const unsigned char *p, size_t n)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < n; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1u) sum += (uint32_t)p[n - 1] << 8;
    sum = (sum >> 16) + (sum & 0xffffu);
    sum += sum >> 16;
    return (uint16_t)~sum;
}

static void finish_packet(unsigned char *out, size_t total)
{
    put16(out + 0, 0x0100u);
    put16(out + 6, 0);
    put16(out + 2, cam_gencp_checksum(out + 6, 10));
    put16(out + 4, cam_gencp_checksum(out + 6, total - 6));
}

/* VisionKitApi.dll 0x180029fa0 */
int cam_gencp_build_read(uint16_t req_id, uint64_t addr, uint16_t len,
                         unsigned char *out, size_t capacity)
{
    if (!out || !len || len > CAM_GENCP_MAX_DATA) return -EINVAL;
    if (capacity < 28) return -ENOSPC;
    put16(out + 8, GENCP_READ_FLAGS);
    put16(out + 10, GENCP_READ_CMD);
    put16(out + 12, 12);
    put16(out + 14, req_id);
    put64(out + 16, addr);
    put16(out + 24, 0);
    put16(out + 26, len);
    finish_packet(out, 28);
    return 28;
}

/* VisionKitApi.dll 0x18002a180 */
int cam_gencp_build_write(uint16_t req_id, uint64_t addr, const void *data, uint16_t len,
                          unsigned char *out, size_t capacity)
{
    if (!out || !data || !len || len > CAM_GENCP_MAX_DATA) return -EINVAL;
    if (capacity < 24u + len) return -ENOSPC;
    put16(out + 8, GENCP_WRITE_FLAGS);
    put16(out + 10, GENCP_WRITE_CMD);
    put16(out + 12, 8u + len);
    put16(out + 14, req_id);
    put64(out + 16, addr);
    memcpy(out + 24, data, len);
    finish_packet(out, 24u + len);
    return (int)(24u + len);
}

static uint16_t next_req(struct cam_serial *c)
{
    if (++c->gencp_req == 0) c->gencp_req = 1;   /* VisionKit 也跳过 0 */
    return c->gencp_req;
}

/* 从串口字节流里拼出一个完整 GenCP 包，返回它的总字节数。 */
static int next_packet(struct cam_serial *c, uint64_t deadline)
{
    struct sg_capture_session *s = c->s;
    uint32_t got;
    size_t need;
    int ret;

    for (;;) {
        if (c->rx_len >= 2 && get16(c->rx) != 0x0100u) {
            c->rx_len = 0;                      /* 不是 GenCP 包头：丢弃并报错，不猜 */
            return -EPROTO;
        }
        need = CAM_GENCP_HDR_BYTES + (c->rx_len >= 14 ? get16(c->rx + 12) : 0u);
        if (c->rx_len >= CAM_GENCP_HDR_BYTES && c->rx_len >= need) return (int)need;
        if (need > CAM_GENCP_RX_CAP - CAM_SERIAL_MAX_PAYLOAD) { c->rx_len = 0; return -EPROTO; }
        {
            uint64_t now = s->io.now_ms(s->io.context);
            if (now >= deadline) return -ETIMEDOUT;
            ret = cam_serial_recv(c, c->rx + c->rx_len, (uint32_t)(CAM_GENCP_RX_CAP - c->rx_len),
                                  &got, (uint32_t)(deadline - now));
        }
        if (ret == -EMSGSIZE) { c->rx_len = 0; return -EPROTO; }
        if (ret) return ret;
        c->rx_len += got;
    }
}

static void consume(struct cam_serial *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
}

/* 发命令、等对应的应答（跳过 PendingAck），把应答 SCD 拷进 scd。返回 SCD 长度。 */
static int transact(struct cam_serial *c, const unsigned char *pkt, int pkt_len,
                    uint16_t req, uint16_t want_cmd, unsigned char *scd, size_t scd_cap,
                    uint32_t timeout_ms)
{
    struct sg_capture_session *s = c->s;
    uint64_t deadline;
    uint16_t cmd, n;
    int ret, total;

    c->rx_len = 0;
    ret = cam_serial_send(c, pkt, (uint32_t)pkt_len);
    if (ret) return ret;
    deadline = s->io.now_ms(s->io.context) + timeout_ms;

    for (;;) {
        total = next_packet(c, deadline);
        if (total < 0) return total;
        cmd = get16(c->rx + 10);
        n = get16(c->rx + 12);
        if (get16(c->rx + 6) != 0 || get16(c->rx + 14) != req) { consume(c, (size_t)total); return -EPROTO; }
        if (cmd == GENCP_PENDING_ACK && n >= 4) {           /* 相机说"还在忙，再等这么久" */
            deadline = s->io.now_ms(s->io.context) + get16(c->rx + 18);
            consume(c, (size_t)total);
            continue;
        }
        if (cmd != want_cmd || n > scd_cap) { consume(c, (size_t)total); return -EPROTO; }
        if (get16(c->rx + 8) != 0) { consume(c, (size_t)total); return -EREMOTEIO; }
        memcpy(scd, c->rx + CAM_GENCP_HDR_BYTES, n);
        consume(c, (size_t)total);
        return n;
    }
}

int cam_gencp_read(struct cam_serial *c, uint64_t addr, void *buf, uint16_t len, uint32_t timeout_ms)
{
    unsigned char pkt[28], scd[CAM_GENCP_MAX_DATA];
    uint16_t req;
    int n;

    if (!c || !c->s || !buf) return -EINVAL;
    req = next_req(c);
    n = cam_gencp_build_read(req, addr, len, pkt, sizeof pkt);
    if (n < 0) return n;
    n = transact(c, pkt, n, req, GENCP_READ_ACK, scd, sizeof scd, timeout_ms);
    if (n < 0) return n;
    if (n != len) return -EPROTO;
    memcpy(buf, scd, len);
    return 0;
}

int cam_gencp_write(struct cam_serial *c, uint64_t addr, const void *data, uint16_t len, uint32_t timeout_ms)
{
    unsigned char pkt[24 + CAM_GENCP_MAX_DATA], scd[4];
    uint16_t req;
    int n;

    if (!c || !c->s) return -EINVAL;
    req = next_req(c);
    n = cam_gencp_build_write(req, addr, data, len, pkt, sizeof pkt);
    if (n < 0) return n;
    n = transact(c, pkt, n, req, GENCP_WRITE_ACK, scd, sizeof scd, timeout_ms);
    if (n < 0) return n;
    /* WriteMemAck 的 SCD：2 字节保留 + 2 字节已写长度（VisionKit 检查 written != 0）*/
    if (n != 4 || get16(scd + 2) != len) return -EPROTO;
    return 0;
}

int cam_gencp_read32(struct cam_serial *c, uint32_t addr, uint32_t *value, uint32_t timeout_ms)
{
    unsigned char b[4];
    int ret;

    if (!value) return -EINVAL;
    ret = cam_gencp_read(c, addr, b, 4, timeout_ms);
    if (ret) return ret;
    *value = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return 0;
}

int cam_gencp_write32(struct cam_serial *c, uint32_t addr, uint32_t value, uint32_t timeout_ms)
{
    unsigned char b[4] = { (unsigned char)(value >> 24), (unsigned char)(value >> 16),
                           (unsigned char)(value >> 8), (unsigned char)value };
    return cam_gencp_write(c, addr, b, 4, timeout_ms);
}
