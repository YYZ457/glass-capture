/* SPDX-License-Identifier: MIT
 *
 * fake_card_test.c —— 假卡测试：用一张"假的回调表"模拟 SG3000 卡（邮箱、块头、DMA），
 * 不碰硬件地验证 board/capture 和 pgm_write 的逻辑。运行：make -C board test
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sg_acq.h"
#include "sg_mailbox.h"
#include "sg_block.h"
#include "sg_cardxml.h"
#include "sg3000_regs.h"
#include "pgm_write.h"
#include "sg_err.h"

#define W 16u
#define MAXL 8u
#define PKT 64u            /* 每包字节数；整块 W*MAXL=128 → 2 包 */

struct card {
    uint32_t tx[8]; unsigned ntx;
    uint32_t rx[80]; unsigned nrx, rxpos, rx_len_bytes;
    int storage_status;
    uint32_t param_addr[8], param_val[8]; unsigned nparam;   /* 写过的卡参数 */                        /* 读卡存储（地址 < 0x10000000）时回复偏移 16 填非 0 */
    unsigned ack;
    int started, stopped, stop_polls_left, stop_drain_hdrs, stop_hdr_stuck, hdr_ready, packets_sent, ack_skip, bad_magic, preload_rx;
    uint32_t block_id;
    uint32_t regs_written_bad;
    unsigned long now;
    int cancel;
    int dma_calls;
    uint32_t pixel_clocks;     /* 0 = 用实测 0x59 */
};
static struct card C;

/* 假卡存储：0x190 起是 URL 字符串，0x4000 起是 10 字节的"XML" */
static const char CARD_URL[] = "Local:SGCard.xml;4000;a?SchemaVersion=1.0.0";
static const char CARD_XML[] = "<xml>12345";
static uint32_t storage_word(uint32_t addr)
{
    uint32_t v = 0, i;
    for (i = 0; i < 4; ++i) {
        uint32_t a = addr + i;
        unsigned char b = 0;
        if (a >= 0x190 && a - 0x190 < sizeof CARD_URL) b = (unsigned char)CARD_URL[a - 0x190];
        if (a >= 0x4000 && a - 0x4000 < sizeof CARD_XML - 1) b = (unsigned char)CARD_XML[a - 0x4000];
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static uint32_t hdr_val(uint32_t off)
{
    unsigned p = C.packets_sent;               /* 当前包 0 或 1 */
    int last = (p == 1);
    switch (off) {
    case SG_HDR_WIDTH: return W;
    case SG_HDR_LINES: return MAXL;
    case SG_HDR_PIXFMT: return SG_PIXFMT_MONO8;
    case SG_HDR_ZERO_A: case SG_HDR_ZERO_B: return 0;
    case SG_HDR_SEQ: return (p + 1) | (last ? 0x80000000u : 0);
    case SG_HDR_PAYLOAD_LEN: return PKT;
    case SG_HDR_DEST_OFFSET: return p * PKT;
    case SG_HDR_SRC_ADDR: return 0x1000u + p * PKT;
    case SG_HDR_BLOCK_ID: return C.block_id;
    case SG_HDR_MAGIC: return C.bad_magic ? 0 : SG_HDR_MAGIC_VALUE;
    case SG_HDR_MAX_LINES: return MAXL;
    default: return 0;
    }
}

static void card_process_mailbox(void)
{
    int write = (C.tx[4] == SG_MBX_OP_WRITE);
    uint32_t addr = C.tx[5], val = 0, len = write ? 0 : C.tx[6], i;
    C.ack += 1 + C.ack_skip; C.ack_skip = 0;
    if (write) {
        if (addr == SG_L_ACQ_START) C.started = 1, C.hdr_ready = 1;
        else if (addr == SG_L_ACQ_STOP) C.stopped = 1;
        else {
            for (i = 0; i < C.nparam && C.param_addr[i] != addr; ++i) {}
            if (i == C.nparam && C.nparam < 8) C.nparam++;
            C.param_addr[i] = addr; C.param_val[i] = C.tx[7];
        }
    } else switch (addr) {
        case SG_L_DDR_STATUS: val = 2; break;
        case SG_L_PIXEL_CLOCKS: val = C.pixel_clocks ? C.pixel_clocks : 0x59; break;
        case SG_L_CL_CONFIG: val = 0; goto param;
        case SG_L_CAMERA_COLOR: val = 0; break;
        case SG_L_CAMERA_DEPTH: val = 8; break;
        case SG_L_CAMERA_WIDTH: val = 4096; goto param;
        case SG_L_CAMERA_HEIGHT: val = 2000; goto param;
        param:
            for (i = 0; i < C.nparam; ++i) if (C.param_addr[i] == addr) val = C.param_val[i];
            break;
        default: val = storage_word(addr); break;
    }
    memset(C.rx, 0, sizeof(C.rx));
    C.rx[0] = 0x000055aa; C.rx[1] = 1;
    C.rx[2] = (write ? 12u : 16u) | (C.ack << 16);
    C.rx[4] = write ? 0x80030000u : 0x80010000u;
    if (!write && C.storage_status && addr < 0x10000000u) C.rx[4] |= 1u;
    C.rx[6] = len;
    if (len == 4) C.rx[7] = val;
    else for (i = 0; i < len / 4 && i < 73; ++i) C.rx[7 + i] = storage_word(addr + 4 * i);
    if (!write) C.rx[2] = (12u + len) | (C.ack << 16);
    C.rx_len_bytes = 28 + len;
    C.nrx = C.rx_len_bytes / 4; C.rxpos = 0;
}

static int f_read(void *ctx, uint32_t off, uint32_t *v)
{
    (void)ctx;
    if (off == SG_MBX_RX_OCCUPANCY) { *v = C.nrx - C.rxpos; return 0; }
    if (off == SG_MBX_TX_ROOM) { *v = 8 - C.ntx; return 0; }
    if (off == SG_MBX_RX_LEN) { *v = C.rx_len_bytes; return 0; }
    if (off == SG_MBX_RX_DATA) { *v = C.rx[C.rxpos++]; return 0; }
    if (off == SG_HDR_READY) {
        if (C.stopped && C.stop_hdr_stuck) { C.now += 1; *v = 1; return 0; }
        if (C.stopped && C.stop_drain_hdrs > 0) { *v = (uint32_t)C.stop_drain_hdrs--; return 0; }
        *v = C.hdr_ready;
        return 0;
    }
    if (off == SG_ACQ_STATE) {
        if (C.stopped && !C.stop_drain_hdrs && C.stop_polls_left > 0) C.stop_polls_left--;
        *v = (C.stopped && !C.stop_hdr_stuck && !C.stop_drain_hdrs && C.stop_polls_left == 0) ? SG_ACQ_STATE_STOP : 0;
        return 0;
    }
    if (off >= 0x202008 && off <= 0x20206c) { *v = hdr_val(off); return 0; }
    if (off == SG_HDR_MAGIC) { *v = hdr_val(off); return 0; }
    return -EINVAL;
}
static int f_write(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    if (off == SG_MBX_TX_DATA) { C.tx[C.ntx++] = v; return 0; }
    if (off == SG_MBX_TX_LEN) { card_process_mailbox(); C.ntx = 0; return 0; }
    if (off == SG_HDR_ACK) { return 0; }
    if (off == SG_ACQ_ARM) { return v == 2 ? 0 : -EINVAL; }
    C.regs_written_bad++; return -EINVAL;
}
static int f_dma(void *ctx, uint32_t src, void *buf, uint32_t len, uint32_t to)
{
    (void)ctx; (void)to;
    C.dma_calls++;
    for (uint32_t i = 0; i < len; ++i) ((unsigned char *)buf)[i] = (unsigned char)((src + i) & 0xff);
    /* 最后一次 DMA 搬完一包，下一包才可见 */
    if (src + len == 0x1000u + PKT) C.packets_sent = 1;
    else if (src + len == 0x1000u + 2 * PKT) { C.packets_sent = 0; C.hdr_ready = 0; C.block_id++; }
    return 0;
}
static uint64_t f_now(void *ctx) { (void)ctx; return C.now; }
static void f_sleep(void *ctx, uint32_t ms) { (void)ctx; C.now += ms; }
static int f_cancel(void *ctx) { (void)ctx; return C.cancel; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void reset(void) { memset(&C, 0, sizeof C); C.block_id = 7; }
static struct sg_capture_io IO = { NULL, f_read, f_write, f_dma, f_now, f_sleep, f_cancel };

int main(void)
{
    struct sg_capture_session s; struct sg_capture_options o; uint32_t v;
    sg_capture_default_options(&o);
    CHECK(o.control_timeout_ms == 1000 && o.frame_timeout_ms == 10000 && o.dma_timeout_ms == 1000);

    /* 1. 会话初始化：缺回调 → -EINVAL */
    struct sg_capture_io bad = IO; bad.dma_read = NULL;
    CHECK(sg_capture_session_init(&s, &bad) == -EINVAL);
    bad = IO; bad.cancelled = NULL;
    CHECK(sg_capture_session_init(&s, &bad) == 0 && sg_session_cancelled(&s) == 0);

    /* 2. 邮箱：连读两次 DDR，值=2，ACK 连续 */
    reset(); CHECK(sg_capture_session_init(&s, &IO) == 0);
    CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == 0 && v == 2);
    CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == 0 && v == 2);
    CHECK(s.request_id == 2 && s.last_ack == 2 && s.have_ack);
    /* 请求报文内容 */
    CHECK(C.tx[0] == 0x000155aa && C.tx[4] == 0x80000000u && C.tx[5] == SG_L_DDR_STATUS && C.tx[6] == 4);
    /* 白名单 */
    CHECK(sg_mbx_write(&s, 0x12345678u, 1000, 0) == -EPERM);
    CHECK(C.ntx == 0 && C.started == 0);
    /* ACK 不连续 → EPROTO */
    C.ack_skip = 1; CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == -EPROTO);
    /* 回复窗口有残留 → EBUSY，且没有往发送窗口写 */
    reset(); sg_capture_session_init(&s, &IO); C.nrx = 8; C.rxpos = 0;
    CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == -EBUSY && C.ntx == 0);
    /* 取消：非 cleanup → ECANCELED；cleanup → 照常 */
    reset(); sg_capture_session_init(&s, &IO); C.cancel = 1;
    CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == -ECANCELED);
    CHECK(sg_mbx_write(&s, SG_L_ACQ_STOP, 1000, 1) == 0 && C.stopped);

    /* 3. 预检 */
    reset(); sg_capture_session_init(&s, &IO);
    struct sg_capture_preflight p;
    CHECK(sg_capture_preflight(&s, 1000, &p) == 0);
    CHECK(p.ddr_status == 2 && p.pixel_clocks == 0x59 && p.camera_depth == 8 && p.valid_mask == 31);
    CHECK(sg_capture_preflight(&s, 0, &p) == -EINVAL && sg_capture_preflight(&s, 2001, &p) == -EINVAL);
    /* EightyBit：2026-11-14 实板只接一根线，时钟 0x19（只有第 1 路）→ ENOLINK，缺第 2、3 路 */
    CHECK(sg_mbx_write_value(&s, SG_L_CL_CONFIG, 4, 1000) == 0);
    C.pixel_clocks = 0x19;
    CHECK(sg_capture_preflight(&s, 1000, &p) == -ENOLINK && p.cl_configuration == 4 && p.valid_mask == 31);
    CHECK(sg_preflight_missing_clocks(&p) == 0x6);
    C.pixel_clocks = 0x1f;                                   /* 两根线都接上 */
    CHECK(sg_capture_preflight(&s, 1000, &p) == 0);
    CHECK(sg_mbx_write_value(&s, SG_L_CL_CONFIG, 2, 1000) == 0);   /* Full：不支持 */
    CHECK(sg_capture_preflight(&s, 1000, &p) == -ENOTSUP);
    CHECK(sg_mbx_write_value(&s, SG_L_CL_CONFIG, 0, 1000) == 0);
    C.pixel_clocks = 0x18;                                   /* Base 没有第 1 路时钟 */
    CHECK(sg_capture_preflight(&s, 1000, &p) == -ENOLINK && sg_preflight_missing_clocks(&p) == 1);
    C.pixel_clocks = 0;
    CHECK(sg_preflight_required_clocks(1) == 0 && sg_preflight_required_clocks(0) == 1 && sg_preflight_required_clocks(4) == 7);

    /* 3b. 读卡存储（sg_cardxml）：URL 字节顺序、解析、按地址读出文件 */
    {
        char url[SG_CARDXML_URL_LEN + 1], name[64], xml[16];
        uint32_t a, l;
        reset(); sg_capture_session_init(&s, &IO);
        CHECK(sg_cardxml_read(&s, SG_CARDXML_URL_ADDR(0), url, SG_CARDXML_URL_LEN, 1000) == 0);
        url[SG_CARDXML_URL_LEN] = 0;
        CHECK(!strcmp(url, CARD_URL));
        CHECK(sg_cardxml_parse_url(url, name, sizeof name, &a, &l) == SG_CARDXML_LOCAL && a == 0x4000 && l == 10);
        memset(xml, 0x5a, sizeof xml);
        CHECK(sg_cardxml_read(&s, a, xml, l, 1000) == 0 && !memcmp(xml, CARD_XML, 10) && xml[10] == 0x5a);
        CHECK(s.request_id == 2 && C.regs_written_bad == 0);   /* URL 256B 1 包；XML 10B 也按 256B 读 1 包，只拷前 10B */
        CHECK(C.tx[6] == SG_MBX_BLOCK_MAX);
        CHECK(!memcmp(s.last_reply, "\xaa\x55\x00\x00", 4) && s.last_reply_len == 28u + SG_MBX_BLOCK_MAX);
        /* 多包 + 非 4 字节对齐尾巴（如实板 38217 = 149*256 + 73）：每包都请求 256B 且不越界覆盖哨兵 */
        {
            unsigned char big[SG_MBX_BLOCK_MAX + 74u];
            memset(big, 0x5a, sizeof big);
            CHECK(sg_cardxml_read(&s, a, big, SG_MBX_BLOCK_MAX + 73u, 1000) == 0);
            CHECK(!memcmp(big, CARD_XML, 10) && big[SG_MBX_BLOCK_MAX + 73u] == 0x5a);
            CHECK(s.request_id == 4 && C.tx[6] == SG_MBX_BLOCK_MAX);
        }
        CHECK(sg_cardxml_read(&s, 0x191, xml, 4, 1000) == -EINVAL);
        /* 实板现象：卡存储回复偏移 16 非 0 → 严格读 -EPROTO，块读（Windows 的检查）照常 */
        reset(); sg_capture_session_init(&s, &IO); C.storage_status = 1;
        CHECK(sg_mbx_read(&s, 0x190, &v, 1000, 0) == -EPROTO);
        CHECK(sg_cardxml_read(&s, SG_CARDXML_URL_ADDR(0), url, SG_CARDXML_URL_LEN, 1000) == 0);
        CHECK(!strcmp(url, CARD_URL));
        CHECK(sg_mbx_read(&s, SG_L_DDR_STATUS, &v, 1000, 0) == 0 && v == 2);   /* ACK 仍连续 */
        CHECK(sg_mbx_read_block(&s, 0x190, xml, 6, 1000) == -EINVAL);
    }

    /* 3c. 写卡参数：白名单、值域、写入格式（第 7 字 = 值）、读回 */
    reset(); sg_capture_session_init(&s, &IO);
    CHECK(sg_mbx_read(&s, SG_L_CAMERA_WIDTH, &v, 1000, 0) == 0 && v == 4096);
    CHECK(sg_mbx_write_value(&s, SG_L_CAMERA_WIDTH, 16384, 1000) == 0);
    CHECK(C.tx[4] == SG_MBX_OP_WRITE && C.tx[5] == SG_L_CAMERA_WIDTH && C.tx[6] == 4 && C.tx[7] == 16384);
    CHECK(sg_mbx_read(&s, SG_L_CAMERA_WIDTH, &v, 1000, 0) == 0 && v == 16384);
    CHECK(sg_mbx_write_value(&s, SG_L_CAMERA_WIDTH, 100, 1000) == -EPERM);          /* < 128 */
    CHECK(sg_mbx_write_value(&s, SG_L_CL_CONFIG, 3, 1000) == -EPERM);               /* 不是 0/1/2/4 */
    CHECK(sg_mbx_write_value(&s, 0x10540018u, 1, 1000) == -EPERM);                 /* DeviceReboot */
    CHECK(sg_mbx_write_value(&s, SG_L_ACQ_START, 1, 1000) == -EPERM);               /* 只能走 sg_mbx_write */
    CHECK(sg_mbx_write(&s, SG_L_CAMERA_WIDTH, 1000, 0) == -EPERM);
    CHECK(sg_mbx_write_allowed(SG_L_TAP_GEOMETRY, 3) && !sg_mbx_write_allowed(SG_L_TAP_GEOMETRY, 11));
    CHECK(sg_mbx_write_allowed(SG_L_ACQ_STOP, 1) && !sg_mbx_write_allowed(SG_L_ACQ_STOP, 2));
    CHECK(C.started == 0 && C.regs_written_bad == 0);

    /* 4. 块头检查（纯函数） */
    struct sg_block_header h = { .width = W, .lines = MAXL, .pixfmt = SG_PIXFMT_MONO8, .seq = 1,
        .payload_len = PKT, .dest_offset = 0, .src_addr = 0x1000, .block_id = 3,
        .magic = SG_HDR_MAGIC_VALUE, .max_lines = MAXL };
    CHECK(sg_block_header_check(&h, 1, 0) == 0);
    CHECK(sg_block_header_check(&h, 2, 0) == -EPROTO);
    CHECK(sg_block_header_check(&h, 1, 8) == -EPROTO);
    h.magic = 1; CHECK(sg_block_header_check(&h, 1, 0) == -EPROTO); h.magic = SG_HDR_MAGIC_VALUE;
    h.lines = MAXL + 1; CHECK(sg_block_header_check(&h, 1, 0) == -EPROTO); h.lines = MAXL;
    h.pixfmt = 0; CHECK(sg_block_header_check(&h, 1, 0) == -EPROTO);

    /* 5. 单帧：begin+next+end，2 包拼成 16x8 */
    reset(); sg_capture_session_init(&s, &IO);
    struct sg_capture_result r;
    CHECK(sg_capture_once(&s, &o, &r) == 0);
    CHECK(r.frame.width == W && r.frame.height == MAXL && r.frame.length == W * MAXL);
    CHECK(r.frame.block_id == 7 && r.frame.packets == 2 && r.stop_error == 0 && r.start_attempted && r.stop_attempted);
    CHECK(r.primary_stage == NULL && r.stop_stage == NULL);
    CHECK(C.started && C.stopped && C.dma_calls == 2 && C.regs_written_bad == 0);
    int ok = 1; for (size_t i = 0; i < r.frame.length; ++i) ok &= r.frame.data[i] == (unsigned char)(0x1000 + i);
    CHECK(ok);
    /* 写 PGM */
    char dir[] = "/tmp/sg_fake_XXXXXX", path[256];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/out.pgm", dir);
    unlink(path);
    CHECK(pgm_write(path, &r.frame) == 0);
    struct stat st; CHECK(stat(path, &st) == 0 && st.st_size == (off_t)(strlen("P5\n16 8\n255\n") + 128));
    CHECK(pgm_write(path, &r.frame) == -EEXIST);          /* 不覆盖 */
    CHECK(pgm_write(path, &(struct sg_capture_frame){0}) == -EINVAL);
    unlink(path); rmdir(dir);
    sg_capture_frame_free(&r.frame); CHECK(r.frame.data == NULL);

    /* 6. 块头魔数错 → EPROTO，仍执行 STOP，且帧已释放 */
    reset(); sg_capture_session_init(&s, &IO); C.bad_magic = 1;
    CHECK(sg_capture_once(&s, &o, &r) == -EPROTO);
    CHECK(r.stop_attempted && C.stopped && r.frame.data == NULL && r.primary_error == -EPROTO);
    CHECK(r.primary_stage && !strcmp(r.primary_stage, "check block header") && r.stop_stage == NULL);
    CHECK(s.last_hdr.valid && s.last_hdr.width == W && s.last_hdr.block_id == 7);

    /* 7. 开始前已有块就绪 → EBUSY，且不尝试 START */
    reset(); sg_capture_session_init(&s, &IO); C.hdr_ready = 1;
    CHECK(sg_capture_once(&s, &o, &r) == -EBUSY);
    CHECK(!r.start_attempted && !C.started && !C.stopped);

    /* 8. 超时：块永远不就绪 */
    reset(); sg_capture_session_init(&s, &IO);
    struct sg_capture_stream st2; CHECK(sg_stream_begin(&st2, &s, &o) == 0);
    C.hdr_ready = 0; struct sg_capture_frame fr = {0};
    CHECK(sg_stream_next(&st2, &fr) == -ETIMEDOUT && st2.failed);
    CHECK(sg_stream_next(&st2, &fr) == -EINVAL);
    CHECK(sg_stream_end(&st2) == 0 && C.stopped);
    CHECK(sg_stream_end(&st2) == 0);                       /* 幂等 */

    /* 9. 连续两块，块号 7、8 连续；再制造不连续 */
    reset(); sg_capture_session_init(&s, &IO);
    CHECK(sg_stream_begin(&st2, &s, &o) == 0);
    CHECK(sg_stream_next(&st2, &fr) == 0); sg_capture_frame_free(&fr);
    C.hdr_ready = 1;
    CHECK(sg_stream_next(&st2, &fr) == 0 && fr.block_id == 8 && st2.id_discontinuities == 0); sg_capture_frame_free(&fr);
    C.hdr_ready = 1; C.block_id = 20;
    CHECK(sg_stream_next(&st2, &fr) == 0 && st2.last_discontinuity && st2.id_discontinuities == 1); sg_capture_frame_free(&fr);
    CHECK(st2.blocks == 3);
    CHECK(sg_stream_end(&st2) == 0);

    /* 10. 容量上限 */
    reset(); sg_capture_session_init(&s, &IO);
    struct sg_capture_options small = o; small.max_frame_bytes = 64;
    CHECK(sg_capture_once(&s, &small, &r) == -EFBIG);
    CHECK(r.primary_stage && !strcmp(r.primary_stage, "block size limit"));

    /* 11. 高带宽停流：STOP 后等 2.5 秒才转成 "STOP"（超过 control_timeout_ms=1000，低于 6000）→ 仍成功 */
    reset(); sg_capture_session_init(&s, &IO);
    C.stop_polls_left = 2500;
    CHECK(sg_capture_once(&s, &o, &r) == 0);
    CHECK(r.frame.width == W && r.frame.height == MAXL && r.stop_error == 0);
    sg_capture_frame_free(&r.frame);

    /* 12. 真正卡死：STOP 后 7 秒仍未停 → 报 -ETIMEDOUT 且定位到 wait SG_ACQ_STATE = STOP */
    reset(); sg_capture_session_init(&s, &IO);
    C.stop_polls_left = 7000;
    CHECK(sg_capture_once(&s, &o, &r) == -ETIMEDOUT);
    CHECK(r.primary_error == 0 && r.stop_error == -ETIMEDOUT);
    CHECK(r.stop_stage && !strcmp(r.stop_stage, "wait SG_ACQ_STATE = STOP") && r.frame.data == NULL);

    /* 13. STOP 后 FIFO 里仍有 7 个未取块头，读 SG_HDR_READY 逐个弹出后立即转成 "STOP" */
    reset(); sg_capture_session_init(&s, &IO);
    C.stop_drain_hdrs = 7;
    C.stop_polls_left = 10;
    CHECK(sg_capture_once(&s, &o, &r) == 0);
    CHECK(C.stop_drain_hdrs == 0 && r.stop_error == 0 && r.frame.width == W && r.frame.height == MAXL);
    sg_capture_frame_free(&r.frame);

    /* 14. SG_HDR_READY 永久卡在非 0 时也不能死循环，仍受 deadline 限制报 -ETIMEDOUT */
    reset(); sg_capture_session_init(&s, &IO);
    C.stop_hdr_stuck = 1;
    CHECK(sg_capture_once(&s, &o, &r) == -ETIMEDOUT);
    CHECK(r.primary_error == 0 && r.stop_error == -ETIMEDOUT && r.frame.data == NULL);

    printf(fails ? "fake card: %d FAILED\n" : "fake card: ALL PASS\n", fails);
    return fails != 0;
}
