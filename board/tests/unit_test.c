/* SPDX-License-Identifier: MIT
 *
 * unit_test.c —— processing / stream / 相机串口的单元测试（不碰硬件）。运行：make -C board test
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sg_processing.h"
#include "sg_candidates.h"
#include "sg_linecheck.h"
#include "sg_pipeline.h"
#include "cam_serial.h"
#include "sg_cardxml.h"
#include "cam_xml.h"
#include "cam_set.h"
#include "sg_pgmcheck.h"
#include "sg_screen.h"
#include "sg3000_regs.h"

static struct sg_image_view view(unsigned char *p, uint32_t w, uint32_t h)
{
    struct sg_image_view v = {0};
    v.pixels = p; v.width = w; v.height = h; v.stride = w; v.bytes = (uint64_t)w * h;
    v.pixel_format = SG_PIXEL_MONO8; v.sequence = 7; v.first_line = 100;
    return v;
}

static void test_view_stats(void)
{
    unsigned char img[16] = {0};
    struct sg_image_view v = view(img, 4, 4);
    struct sg_processor p; struct sg_processing_result r;
    img[5] = 255; img[6] = 10;
    assert(sg_view_validate(&v) == 0);
    v.stride = 3; assert(sg_view_validate(&v) == -EINVAL); v.stride = 4;
    v.bytes = 15; assert(sg_view_validate(&v) == -EINVAL); v.bytes = 16;
    assert(sg_stats_open(&p, 256) == -EINVAL);
    assert(sg_stats_open(&p, 128) == 0);
    assert(p.process(p.context, &v, 1000, &r) == 0);
    assert(r.sum == 265 && r.maximum == 255 && r.minimum == 0 && r.above_threshold == 1);
    assert(r.decision == SG_UNKNOWN && r.sequence == 7 && r.first_line == 100 && r.pixel_count == 16);
    p.close(p.context);
    puts("view/stats ok");
}

static void test_candidates(void)
{
    enum { W = 64, H = 32 };
    static unsigned char img[W * H];
    float mean[W]; struct sg_background bg = { W, mean, NULL };
    struct sg_candidate c[8]; uint32_t n;
    struct sg_candidate_params p = { 20, 2, 0, 0 };
    struct sg_image_view v = view(img, W, H);
    int i;
    memset(img, 50, sizeof img);
    for (i = 0; i < W; ++i) mean[i] = 50;
    /* 亮团：3x3 at (10..12, 5..7)，中心最亮 */
    for (int y = 5; y <= 7; ++y) for (int x = 10; x <= 12; ++x) img[y * W + x] = 100;
    img[6 * W + 11] = 200;
    /* 暗团：对角相连两个像素 (40,20)(41,21) -> 8 邻接应合并 */
    img[20 * W + 40] = 0; img[21 * W + 41] = 0;
    /* 孤立单像素：面积 1 < min_area，丢掉 */
    img[30 * W + 60] = 255;
    /* U 形：两臂从上方分开，底部相连 -> 一个连通域（测试合并） */
    for (int y = 10; y <= 14; ++y) { img[y * W + 30] = 150; img[y * W + 34] = 150; }
    for (int x = 30; x <= 34; ++x) img[14 * W + x] = 150;

    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == 0);
    assert(n == 3);
    assert(c[0].x0 == 10 && c[0].y0 == 5 && c[0].x1 == 13 && c[0].y1 == 8 && c[0].area == 9 && c[0].peak == 150);
    assert(fabsf(c[0].cx - 11) < 1e-4 && fabsf(c[0].cy - 6) < 1e-4);
    assert(c[1].x0 == 30 && c[1].y0 == 10 && c[1].x1 == 35 && c[1].y1 == 15 && c[1].area == 13);
    assert(c[2].x0 == 40 && c[2].y1 == 22 && c[2].area == 2 && c[2].peak == 50);
    /* 截断 */
    p.max_candidates = 2;
    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == SG_CANDIDATES_TRUNCATED && n == 2);
    /* 面积上限 */
    p.max_candidates = 0; p.max_area = 9;
    assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == 0 && n == 2);
    /* 无背景：用自身列均值，仍能找到亮团 */
    p.max_area = 0;
    assert(sg_candidates_find(&v, NULL, &p, c, 8, &n) >= 0 && n >= 1);
    /* 参数错误 */
    p.threshold = 0; assert(sg_candidates_find(&v, &bg, &p, c, 8, &n) == -EINVAL);
    puts("candidates ok");
}

static void test_linecheck(void)
{
    struct sg_linecheck c; struct sg_capture_frame f = {0};
    uint32_t ids[] = {1, 2, 3, 5, 6}; int jumps = 0;
    sg_linecheck_reset(&c);
    f.height = 100;
    for (int i = 0; i < 5; ++i) { f.block_id = ids[i]; jumps += sg_linecheck_feed(&c, &f, (uint64_t)i * 1000000000u); }
    assert(jumps == 1 && c.id_jumps == 1 && c.lines == 500 && c.blocks == 5);
    assert(fabs(sg_linecheck_line_rate(&c) - 100.0) < 1e-9);
    puts("linecheck ok");
}

/* 处理器返回错误 -> 流水线以 processing_error 结束，stop 只调一次 */
static int stops;
static uint32_t nid;
static int paced;
static int src_next(void *c, struct sg_capture_frame *f)
{ (void)c; if (paced) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); } memset(f, 0, sizeof *f); f->data = calloc(1, 64); f->width = 8; f->height = 8; f->length = 64; f->block_id = ++nid; return 0; }
static int src_stop(void *c) { (void)c; stops++; return 0; }
static int bad_process(void *c, const struct sg_image_view *v, uint32_t t, struct sg_processing_result *r)
{ (void)c; (void)v; (void)t; (void)r; return -EIO; }
static int pubs;
static int publish(void *c, const struct sg_image_view *v, const struct sg_processing_result *r, uint32_t t)
{ (void)c; (void)v; (void)r; (void)t; pubs++; return 0; }

static void test_pipeline(void)
{
    struct sg_source s = { NULL, src_next, src_stop };
    struct sg_processor p; struct sg_result_sink k = { NULL, publish };
    struct sg_pipeline_options o = { .queue_depth = 4, .processing_timeout_ms = 1000,
        .max_blocks = 1000, .max_frame_bytes = 4096 };
    struct sg_pipeline_stats st;
    assert(sg_stats_open(&p, 10) == 0);
    paced = 1; o.max_blocks = 300;
    assert(sg_pipeline_run(&s, &p, &k, &o, &st) == 0);
    assert(st.received == 300 && st.processed == 300 && pubs == 300 && st.id_discontinuities == 0);
    paced = 0; o.max_blocks = 1000;
    printf("pipeline normal: ret received=%llu processed=%llu peak=%u qfull=%llu stops=%d\n",
           (unsigned long long)st.received, (unsigned long long)st.processed, st.queue_peak,
           (unsigned long long)st.queue_full_events, stops);
    assert(stops == 1);
    p.close(p.context);
    p.process = bad_process; p.close = NULL; stops = 0;
    int ret = sg_pipeline_run(&s, &p, &k, &o, &st);
    printf("pipeline bad: ret=%d proc_err=%d src_err=%d stops=%d\n", ret, st.processing_error, st.source_error, stops);
    assert(st.processing_error == -EIO || st.source_error == -ENOBUFS);
    assert(ret == -EIO || ret == -ENOBUFS);
    assert(stops == 1);
    puts("pipeline ok");
}

/* ---- 相机串口：假串口邮箱 ---- */
static struct { uint32_t wr_off[64], wr_val[64]; unsigned nwr;
                uint32_t rx[16]; unsigned nrx, pos; uint32_t rx_len; uint64_t now; } SER;
static int ser_rd(void *c, uint32_t off, uint32_t *v)
{
    (void)c;
    if (off == SG_SER_RX_OCCUPANCY(1)) *v = SER.pos < SER.nrx;
    else if (off == SG_SER_RX_LEN(1)) *v = SER.rx_len;
    else if (off == SG_SER_RX_DATA(1)) { assert(SER.pos < SER.nrx); *v = SER.rx[SER.pos++]; }
    else return -EINVAL;
    return 0;
}
static int ser_wr(void *c, uint32_t off, uint32_t v)
{ (void)c; assert(SER.nwr < 64); SER.wr_off[SER.nwr] = off; SER.wr_val[SER.nwr++] = v; return 0; }
static int ser_dma(void *c, uint32_t s, void *b, uint32_t l, uint32_t t) { (void)c; (void)s; (void)b; (void)l; (void)t; return -EIO; }
static uint64_t ser_now(void *c) { (void)c; return SER.now; }
static void ser_sleep(void *c, uint32_t ms) { (void)c; SER.now += ms; }

static uint16_t be16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void test_cam_serial(void)
{
    struct sg_capture_io io = { NULL, ser_rd, ser_wr, ser_dma, ser_now, ser_sleep, NULL, 0 };
    struct sg_capture_session s;
    struct cam_serial c;
    const unsigned char msg[5] = { 0x01, 0x00, 0xaa, 0xbb, 0xcc };
    unsigned char buf[8];
    uint32_t words[8], d, len, hdr[4] = { 0x000055aau, 0, 3, 0 };
    unsigned i;

    /* 分频：规则 (200M + 8*baud) / (16*baud) */
    assert(cam_uart_divisor(115200, &d) == 0 && d == 109);
    assert(cam_uart_divisor(9600, &d) == 0 && d == 1302);
    assert(cam_uart_divisor(460800, &d) == 0 && d == 27);
    assert(cam_uart_divisor(100000, &d) == -EINVAL);

    /* 组包：端口 1 第 1 字 = 0x12，第 2 字 = 长度 | 序号<<16，载荷小端、尾部补 0 */
    assert(cam_serial_pack(1, 7, msg, 5, words, 8) == 6);
    assert(words[0] == 0x000155aau && words[1] == 0x12 && words[2] == (5u | 7u << 16) && words[3] == 0);
    assert(words[4] == 0xbbaa0001u && words[5] == 0xcc);
    assert(cam_serial_pack(1, 7, msg, 5, words, 5) == -ENOSPC);
    assert(cam_serial_pack(2, 7, msg, 5, words, 8) == -EINVAL);
    assert(cam_serial_pack(0, 7, msg, 0, words, 8) == -EINVAL);

    /* 回包头：0x55aa → 取偏移 8；否则 total-16；超长报错 */
    assert(cam_serial_payload_len(hdr, 20, &len) == 0 && len == 3);
    hdr[0] = 0; assert(cam_serial_payload_len(hdr, 22, &len) == 0 && len == 6);
    hdr[0] = 0x55aa; hdr[2] = 9; assert(cam_serial_payload_len(hdr, 20, &len) == -EPROTO);
    assert(cam_serial_payload_len(hdr, 12, &len) == -EPROTO);

    /* 发送：6 个字写 TX_DATA，最后 TX_LEN = 5 + 16 */
    assert(sg_capture_session_init(&s, &io) == 0);
    assert(cam_serial_init(&c, &s, 2) == -EINVAL);
    assert(cam_serial_init(&c, &s, 1) == 0);
    assert(cam_serial_send(&c, msg, 5) == 0);
    assert(SER.nwr == 7);
    for (i = 0; i < 6; ++i) assert(SER.wr_off[i] == 0x413010u);
    assert(SER.wr_val[2] == 5u);          /* 第一包序号是 0 */
    assert(SER.wr_off[6] == 0x413014u && SER.wr_val[6] == 21);

    /* 接收：头 + 2 字载荷（有效 5 字节）*/
    SER.rx[0] = 0x000055aau; SER.rx[1] = 0x12; SER.rx[2] = 5; SER.rx[3] = 0;
    SER.rx[4] = 0x04030201u; SER.rx[5] = 0x05; SER.nrx = 6; SER.pos = 0; SER.rx_len = 24;
    assert(cam_serial_recv(&c, buf, sizeof buf, &len, 100) == 0 && len == 5);
    assert(buf[0] == 1 && buf[3] == 4 && buf[4] == 5 && SER.pos == 6);

    /* 缓冲区太小：整包照样读完，报 -EMSGSIZE */
    SER.pos = 0;
    assert(cam_serial_recv(&c, buf, 2, &len, 100) == -EMSGSIZE && len == 5 && SER.pos == 6);

    /* 没有回包：超时 */
    assert(cam_serial_recv(&c, buf, sizeof buf, &len, 20) == -ETIMEDOUT);

    /* GenCP 校验和：16 位反码求和 */
    {
        const unsigned char ck[4] = { 0x00, 0x01, 0xf2, 0x03 };
        unsigned char pkt[40];
        assert(cam_gencp_checksum(ck, 4) == (uint16_t)~0xf204u);
        assert(cam_gencp_checksum(ck, 3) == (uint16_t)~0xf201u);
        assert(cam_gencp_build_read(0x1234, 0x10010, 4, pkt, sizeof pkt) == 28);
        assert(pkt[0] == 0x01 && pkt[1] == 0x00 && pkt[8] == 0x40 && pkt[9] == 0x01);
        assert(pkt[10] == 0x08 && pkt[11] == 0x00 && pkt[13] == 12 && pkt[14] == 0x12 && pkt[15] == 0x34);
        assert(pkt[21] == 0x01 && pkt[22] == 0x00 && pkt[23] == 0x10 && pkt[27] == 4);
        assert(be16(pkt + 2) == cam_gencp_checksum(pkt + 6, 10));
        assert(be16(pkt + 4) == cam_gencp_checksum(pkt + 6, 22));
        assert(cam_gencp_build_write(7, 0x20, msg, 5, pkt, sizeof pkt) == 29);
        assert(pkt[8] == 0x40 && pkt[9] == 0x00 && pkt[11] == 0x02 && pkt[13] == 13 && pkt[24] == 0x01);
        assert(be16(pkt + 4) == cam_gencp_checksum(pkt + 6, 23));
        assert(cam_gencp_build_write(7, 0x20, msg, 5, pkt, 28) == -ENOSPC);
    }
    puts("cam_serial ok");
}

static void test_cardxml_url(void)
{
    char name[32];
    uint32_t a = 0, l = 0;

    assert(sg_cardxml_parse_url("Local:SGCard.zip;10200000;1A2b?SchemaVersion=1.0.0", name, sizeof name, &a, &l) == SG_CARDXML_LOCAL);
    assert(!strcmp(name, "SGCard.zip") && a == 0x10200000u && l == 0x1a2bu);
    assert(sg_cardxml_parse_url("Local:a b/c.xml;0;10", name, sizeof name, &a, &l) == 0 && !strcmp(name, "a_b_c.xml"));
    assert(sg_cardxml_parse_url("File:SGCard.xml", name, sizeof name, &a, &l) == SG_CARDXML_FILE && !strcmp(name, "SGCard.xml"));
    assert(sg_cardxml_parse_url("Local:x;12", name, sizeof name, &a, &l) == -EPROTO);
    assert(sg_cardxml_parse_url("Local:x;12;0", name, sizeof name, &a, &l) == -EPROTO);
    assert(sg_cardxml_parse_url("Local:x;1g;4", name, sizeof name, &a, &l) == -EPROTO);
    assert(sg_cardxml_parse_url("acoL", name, sizeof name, &a, &l) == -EPROTO);
    assert(SG_CARDXML_URL_ADDR(0) == 0x190u && SG_CARDXML_URL_ADDR(1) == 0x290u);
    puts("cardxml url ok");
}

/* GenCP 清单条目解析（纯函数）。字节布局：文件版本 4、格式版本 4、地址 8、大小 8、SHA1 20，全部大端。 */
static void test_cam_manifest(void)
{
    unsigned char b[CAM_MANIFEST_ENTRY_BYTES];
    struct cam_manifest_entry e;
    const unsigned char be[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    assert(cam_be64(be) == 0x0102030405060708ull);
    memset(b, 0, sizeof b);
    b[0] = 1; b[1] = 2; b[2] = 0; b[3] = 3;          /* 文件版本 1.2.3 */
    b[4] = 0x01; b[5] = 0x01; b[6] = 0x04; b[7] = 0;  /* 格式版本 1.1，bit10 = 1 → zip */
    b[13] = 0x10; b[14] = 0x00;                        /* 地址 0x100000 */
    b[21] = 0x01; b[22] = 0x86; b[23] = 0xa0;          /* 大小 100000 */
    assert(cam_manifest_parse(b, &e) == 0);
    assert(e.file_version == 0x01020003u && e.address == 0x100000u && e.size == 100000u);
    assert(!strcmp(cam_manifest_ext(&e), ".zip") && !cam_manifest_has_sha1(&e));
    b[30] = 0xab;
    assert(cam_manifest_parse(b, &e) == 0 && cam_manifest_has_sha1(&e) && e.sha1[6] == 0xab);
    b[6] = 0x00;                                        /* 类型 0 → xml */
    assert(cam_manifest_parse(b, &e) == 0 && !strcmp(cam_manifest_ext(&e), ".xml"));
    b[6] = 0x08;                                        /* 类型 2：不认识 */
    assert(cam_manifest_parse(b, &e) == -EPROTO);
    b[6] = 0x04;
    b[21] = b[22] = b[23] = 0;                          /* 大小 0 */
    assert(cam_manifest_parse(b, &e) == -EPROTO);
    b[20] = 0x01;                                       /* 16 MiB > 上限 */
    assert(cam_manifest_parse(b, &e) == -EPROTO);
    memset(b + 8, 0xff, 8); b[20] = 0; b[23] = 0x10;    /* 地址 + 大小溢出 */
    assert(cam_manifest_parse(b, &e) == -EPROTO);
    puts("cam manifest ok");
}

/* camera-set 白名单与换算（纯函数）。值域和锁定条件照 16K 相机 XML。 */
static void test_cam_set(void)
{
    /* Base、曝光 1..93.75 µs、行周期 5900..960000 时钟（约 100 Hz..16.27 kHz）、内触发 */
    struct cam_set_state base = { .cl_config = 0, .pixel_format = CAM16K_MONO8, .exposure_min_clk = 96,
                                  .exposure_max_clk = 9000, .line_period_min_clk = 5900, .line_period_max_clk = 960000 };
    struct cam_set_state deca = base;
    uint32_t raw = 0;
    const char *why;

    deca.cl_config = 3;
    assert(cam_set_address("UserSetSave") == 0 && cam_set_address("TestPattern") == CAM16K_TEST_PATTERN);
    assert(cam_set_address("FFCUserSetSave") == 0 && cam_set_address("DeviceReset") == 0);   /* 永远不放开 */
    assert(cam_set_check("ClConfiguration", "0", &deca, &raw, &why) == 0 && raw == 0);
    assert(cam_set_check("ClConfiguration", "4", &deca, &raw, &why) == -EPERM);   /* 相机的 EightyBit 是 3 */
    assert(cam_set_check("DeviceTapGeometry", "3", &base, &raw, &why) == 0 && raw == 3);
    assert(cam_set_check("DeviceTapGeometry", "3", &deca, &raw, &why) == -EPERM && why);   /* EightyBit 锁定 */
    assert(cam_set_check("DeviceTapGeometry", "10", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("TestPattern", "3", &base, &raw, &why) == 0 && raw == 3);
    assert(cam_set_check("TestPattern", "5", &base, &raw, &why) == 0);
    assert(cam_set_check("TestPattern", "4", &base, &raw, &why) == -EPERM);        /* 16K XML 没有 4、6 */
    assert(cam_set_check("TestPattern", "6", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("TestPattern", "x", &base, &raw, &why) == -EINVAL);
    assert(cam_set_check("TestPattern", "-1", &base, &raw, &why) == -EINVAL);
    assert(cam_set_check("ExposureTime", "50", &base, &raw, &why) == 0 && raw == 4800);  /* 实测默认 */
    assert(cam_set_check("ExposureTime", "80.5", &base, &raw, &why) == 0 && raw == 7728);
    assert(cam_set_check("ExposureTime", "95", &base, &raw, &why) == -EPERM);      /* 超过相机给的上限 */
    assert(cam_set_check("ExposureTime", "0.5", &base, &raw, &why) == -EPERM);     /* 低于下限 */
    {
        struct cam_set_state bad = base;
        bad.exposure_min_clk = 0;
        assert(cam_set_check("ExposureTime", "50", &bad, &raw, &why) == -EPERM);   /* 相机没给范围就不写 */
    }
    assert(cam_set_check("DigitalGain", "1", &base, &raw, &why) == 0 && raw == 256);
    assert(cam_set_check("DigitalGain", "2.5", &base, &raw, &why) == 0 && raw == 640);
    assert(cam_set_check("DigitalGain", "16", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("DigitalGain", "0.05", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("AnalogGain", "4", &base, &raw, &why) == 0 && raw == 4);
    assert(cam_set_check("AnalogGain", "5", &base, &raw, &why) == -EPERM);
    {
        struct cam_set_state binned = base;
        binned.binning_v = 2;
        assert(cam_set_check("AnalogGain", "1", &binned, &raw, &why) == -EPERM);   /* SensorBinVerLock */
    }
    assert(cam_set_check("LineRate", "10000", &base, &raw, &why) == 0 && raw == 9600);    /* 实测默认 */
    assert(cam_set_check("LineRate", "15000", &base, &raw, &why) == 0 && raw == 6400);    /* 最终目标 */
    assert(cam_set_check("LineRate", "20000", &base, &raw, &why) == -EPERM);              /* 超过相机上限 */
    assert(cam_set_check("LineRate", "50", &base, &raw, &why) == -EPERM);
    {
        struct cam_set_state trig = base;
        trig.line_trigger_mode = 1;
        assert(cam_set_check("LineRate", "15000", &trig, &raw, &why) == -EPERM);          /* 外触发时锁定 */
    }
    /* 位移、TDI、黑电平 */
    assert(cam_set_check("DigitalShift", "2", &base, &raw, &why) == 0 && raw == 2);
    assert(cam_set_check("DigitalShift", "5", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("TDIStage", "1", &base, &raw, &why) == 0 && raw == 1);
    assert(cam_set_check("TDIStage", "2", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("TDIMode", "1", &base, &raw, &why) == 0 && cam_set_check("TDIMode", "2", &base, &raw, &why) == -EPERM);
    {
        struct cam_set_state black = base;
        assert(cam_set_check("BlackLevel", "0", &black, &raw, &why) == 0 && raw == 0);     /* 范围 0..0 */
        black.black_min = -64; black.black_max = 63;
        assert(cam_set_check("BlackLevel", "-5", &black, &raw, &why) == 0 && raw == 0xfffffffbu);
        assert(cam_set_check("BlackLevel", "64", &black, &raw, &why) == -EPERM);
        assert(cam_set_check("BlackLevel", "-65", &black, &raw, &why) == -EPERM);
        assert(cam_set_check("BlackLevel", "1.5", &black, &raw, &why) == -EINVAL);
        black.black_min = 5; black.black_max = 1;
        assert(cam_set_check("BlackLevel", "3", &black, &raw, &why) == -EPERM);            /* 相机给的范围不成立 */
    }
    /* 平场校正：开关、选组、目标；两个校正命令要先选到用户组 1..3，且不能镜像 */
    assert(cam_set_check("FFCEnable", "1", &base, &raw, &why) == 0 && raw == 1);
    assert(cam_set_check("FFCEnable", "2", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("FFCUserSetSelector", "3", &base, &raw, &why) == 0 && cam_set_check("FFCUserSetSelector", "4", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("FFCSampleFrames", "2048", &base, &raw, &why) == 0 && raw == 2048);
    assert(cam_set_check("FFCSampleFrames", "512", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("FFCCalTargetMode", "2", &base, &raw, &why) == 0 && cam_set_check("FFCCalTargetMode", "3", &base, &raw, &why) == -EPERM);
    assert(cam_set_check("FFCCalTargetValue", "180", &base, &raw, &why) == -EPERM && why);  /* 目标模式不是 2 */
    assert(!cam_set_is_command("FFCEnable") && cam_set_is_command("FFCCalFPN") && cam_set_is_command("FFCCalPRNU") &&
           cam_set_is_command("FFCUserSetLoad") && !cam_set_is_command("nonsense"));
    assert(cam_set_check("FFCCalFPN", "1", &base, &raw, &why) == -EPERM && why);            /* 出厂组：锁定 */
    assert(cam_set_check("FFCUserSetLoad", "1", &base, &raw, &why) == 0 && raw == 1);
    assert(cam_set_check("FFCUserSetLoad", "0", &base, &raw, &why) == -EPERM);
    {
        struct cam_set_state ffc = base;
        ffc.ffc_user_set = 1; ffc.ffc_target_mode = 2;
        assert(!cam_set_ffc_cal_locked(&ffc));
        assert(cam_set_check("FFCCalFPN", "1", &ffc, &raw, &why) == 0 && raw == 1);
        assert(cam_set_check("FFCCalPRNU", "1", &ffc, &raw, &why) == 0 && raw == 1);
        assert(cam_set_check("FFCCalPRNU", "2", &ffc, &raw, &why) == -EPERM);
        assert(cam_set_check("FFCCalTargetValue", "180", &ffc, &raw, &why) == 0 && raw == 2880);
        assert(cam_set_check("FFCCalTargetValue", "251", &ffc, &raw, &why) == -EPERM);
        ffc.reverse_x = 1;
        assert(cam_set_ffc_cal_locked(&ffc) && cam_set_check("FFCCalPRNU", "1", &ffc, &raw, &why) == -EPERM);
        ffc.reverse_x = 0; ffc.ffc_user_set = 0; ffc.ffc_factory = 1;
        assert(cam_set_check("FFCCalFPN", "1", &ffc, &raw, &why) == 0);                     /* 工厂模式标志位 */
    }
    assert(cam_set_state_addr[0] == CAM16K_CL_CONFIG && cam_set_state_addr[7] == CAM16K_LINE_TRIGGER_MODE &&
           cam_set_state_addr[13] == CAM16K_BLACK_LEVEL_MAX);
    puts("cam set ok");
}

/* 板上版 V1：渐变公式、位统计、亮度分位、缩略图。 */
static void test_pgmcheck(void)
{
    enum { W = 768, H = 4 };
    static uint8_t img[W * H], thumb[(W / 16) * 1];
    struct sg_pgm_check c;
    uint32_t x, y, tw, th;

    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) img[y * W + x] = (uint8_t)x;
    assert(sg_pgm_check(img, W, H, &c) == 0);
    assert(c.or_all == 0xff && c.min == 0 && c.max == 255 && c.ramp_mismatch[SG_RAMP_X] == 0);
    assert(c.ramp_mismatch[SG_RAMP_ROW_START] == 0 && c.ramp_mismatch[SG_RAMP_SCALED] != 0 && !c.all_zero);
    assert(c.saturated == 3u * H && c.p50 == 127);
    for (x = 0; x < W * H; ++x) img[x] &= 0x3f;              /* 最高两位丢了 */
    assert(sg_pgm_check(img, W, H, &c) == 0 && c.or_all == 0x3f && c.max == 63 && c.ramp_mismatch[SG_RAMP_X] != 0);
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) img[y * W + x] = (uint8_t)(x + 7 * y);   /* 移动渐变 */
    assert(sg_pgm_check(img, W, H, &c) == 0 && c.ramp_mismatch[SG_RAMP_ROW_START] == 0 && c.ramp_mismatch[SG_RAMP_X] != 0);
    memset(img, 255, sizeof img);
    assert(sg_pgm_check(img, W, H, &c) == 0 && c.all_255 && c.saturated == (uint64_t)W * H);
    for (x = 0; x < W * H; ++x) img[x] = (uint8_t)(x % W < 384 ? 0 : 200);
    {
        static uint8_t t2[(W / 4) * (H / 4)];
        assert(sg_pgm_thumb(img, W, H, 4, t2, &tw, &th) == 0 && tw == W / 4 && th == 1);
        assert(t2[0] == 0 && t2[tw - 1] == 200);
    }
    assert(sg_pgm_thumb(img, W, H, 0, thumb, &tw, &th) == -EINVAL);
    assert(sg_pgm_check(NULL, W, H, &c) == -EINVAL);
    puts("pgmcheck ok");
}


static uint32_t test_rng_state = 1;
static uint32_t test_rng(void)
{
    test_rng_state = test_rng_state * 1664525u + 1013904223u;
    return test_rng_state >> 8;
}

/* 提速后的 sg_stats_row（ARM64 上是 NEON）必须和逐像素的笨办法完全一样，各种长度都要对。 */
static void test_stats_row(void)
{
    enum { N = 5000 };
    static unsigned char buf[N];
    static const uint32_t lengths[] = { 0, 1, 15, 16, 17, 31, 255, 2047, 2048, 2049, 4096, 4999, N };
    static const uint32_t thresholds[] = { 0, 1, 127, 128, 254, 255 };
    unsigned li, ti, mode;
    uint32_t x;

    for (mode = 0; mode < 3; ++mode) {
        for (x = 0; x < N; ++x) buf[x] = mode == 0 ? (unsigned char)test_rng() : mode == 1 ? 255 : 0;
        for (li = 0; li < sizeof lengths / sizeof lengths[0]; ++li) {
            for (ti = 0; ti < sizeof thresholds / sizeof thresholds[0]; ++ti) {
                uint64_t sum = 5, above = 7, rsum = 5, rabove = 7;
                uint32_t min = 200, max = 3, rmin = 200, rmax = 3;

                for (x = 0; x < lengths[li]; ++x) {
                    rsum += buf[x];
                    rabove += buf[x] > thresholds[ti];
                    if (buf[x] < rmin) rmin = buf[x];
                    if (buf[x] > rmax) rmax = buf[x];
                }
                sg_stats_row(buf, lengths[li], thresholds[ti], &sum, &above, &min, &max);
                assert(sum == rsum && above == rabove && min == rmin && max == rmax);
            }
        }
    }
    {   /* 经处理器走一遍：宽度不是 16 的倍数、stride 比宽度大 */
        enum { W = 1003, H = 7, STRIDE = 1024 };
        static unsigned char img[STRIDE * H];
        struct sg_image_view v = view(img, W, H);
        struct sg_processor p; struct sg_processing_result r;
        uint64_t rsum = 0, rabove = 0; uint32_t rmin = 255, rmax = 0, y;

        v.stride = STRIDE; v.bytes = (uint64_t)STRIDE * H;
        for (x = 0; x < STRIDE * H; ++x) img[x] = (unsigned char)test_rng();
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            unsigned char px = img[y * STRIDE + x];
            rsum += px; rabove += px > 100;
            if (px < rmin) rmin = px;
            if (px > rmax) rmax = px;
        }
        assert(sg_stats_open(&p, 100) == 0);
        assert(p.process(p.context, &v, 1000, &r) == 0);
        assert(r.sum == rsum && r.above_threshold == rabove && r.minimum == rmin && r.maximum == rmax);
        p.close(p.context);
    }
    puts("stats row ok");
}

/* 快筛：和逐像素的笨办法比 run 列表；截断、参数检查、"永不报"的边界值。 */
static void test_screen(void)
{
    enum { W = 333, H = 40, STRIDE = 340, CAP = W * H };
    static uint8_t img[STRIDE * H], lo[W], hi[W];
    static struct sg_run got[CAP], want[CAP];
    struct sg_screen_stats st;
    uint32_t count, n, x, y, round;

    for (round = 0; round < 6; ++round) {
        uint64_t dark = 0, bright = 0;

        for (x = 0; x < STRIDE * H; ++x) {
            uint32_t q = test_rng();
            /* round 0–2：大多数像素不命中，偶尔一小段命中（像真实图）；3–5：到处都是命中 */
            img[x] = round < 3 ? (uint8_t)(128 + (q % 5) - 2 + ((q >> 8) % 97 == 0 ? 60 : 0) - ((q >> 16) % 89 == 0 ? 60 : 0))
                               : (uint8_t)q;
        }
        for (x = 0; x < W; ++x) {
            lo[x] = round < 3 ? 120 : (uint8_t)(test_rng() % 128);
            hi[x] = round < 3 ? 136 : (uint8_t)(128 + test_rng() % 128);
        }
        if (round == 5) for (x = 0; x < W; ++x) { lo[x] = 0; hi[x] = 255; }     /* 永不报 */
        for (n = 0, y = 0; y < H; ++y) {
            int cur = 0; uint32_t x0 = 0;
            for (x = 0; x <= W; ++x) {
                uint8_t px = x < W ? img[y * STRIDE + x] : 0;
                int cls = x == W ? 0 : px < lo[x] ? 1 : px > hi[x] ? 2 : 0;
                if (cur && cls != cur) {
                    want[n].y = y; want[n].x0 = (uint16_t)x0; want[n].x1 = (uint16_t)x; want[n].bright = cur == 2;
                    if (cur == 2) bright += x - x0; else dark += x - x0;
                    n++; cur = 0;
                }
                if (cls && !cur) { cur = cls; x0 = x; }
            }
        }
        assert(sg_screen(img, W, H, STRIDE, lo, hi, got, CAP, &count, &st) == 0);
        assert(count == n && st.runs == n && st.dark_pixels == dark && st.bright_pixels == bright);
        for (x = 0; x < n; ++x)
            assert(got[x].y == want[x].y && got[x].x0 == want[x].x0 && got[x].x1 == want[x].x1 && got[x].bright == want[x].bright);
        assert(round != 5 || n == 0);
        assert(round >= 3 || (n > 20 && n < W * H / 20));
        if (n > 3) {                 /* 容量不够：前 3 个照写，其余只计数 */
            assert(sg_screen(img, W, H, STRIDE, lo, hi, got, 3, &count, &st) == SG_SCREEN_TRUNCATED);
            assert(count == 3 && st.runs == n && st.dark_pixels == dark && got[2].x0 == want[2].x0);
            assert(sg_screen(img, W, H, STRIDE, lo, hi, NULL, 0, &count, &st) == SG_SCREEN_TRUNCATED && count == 0);
        }
    }
    assert(sg_screen(img, 0, H, STRIDE, lo, hi, got, CAP, &count, &st) == -EINVAL);
    assert(sg_screen(img, W, H, W - 1, lo, hi, got, CAP, &count, &st) == -EINVAL);
    assert(sg_screen(img, W, H, STRIDE, lo, hi, NULL, 5, &count, &st) == -EINVAL);
    assert(sg_screen(NULL, W, H, STRIDE, lo, hi, got, CAP, &count, &st) == -EINVAL);
    assert(!strcmp(sg_screen_backend(), "neon") || !strcmp(sg_screen_backend(), "generic"));
    puts("screen ok");
}

/* 噪声模型：造一张"静止场景"图，四种亮度、噪声方差随亮度线性增加，看分档和拟合。 */
static void test_pgm_noise(void)
{
    enum { W = 4 * 96, H = 1500 };
    static const uint32_t level[4] = { 10, 40, 100, 200 };
    static const uint32_t spread[4] = { 1, 2, 3, 4 };       /* 均匀噪声 ±spread：方差 = s(s+1)/3 */
    static uint8_t img[W * H];
    struct sg_pgm_noise nz;
    uint32_t x, y, b, seen = 0;

    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
        uint32_t g = x / 96;
        img[y * W + x] = (uint8_t)(level[g] + test_rng() % (2 * spread[g] + 1) - spread[g]);
    }
    assert(sg_pgm_noise(img, W, H, &nz) == 0);
    for (b = 0; b < SG_NOISE_BINS; ++b) {
        uint32_t g;
        if (!nz.bin[b].columns) continue;
        for (g = 0; g < 4 && !(level[g] >= nz.bin[b].lo && level[g] < nz.bin[b].hi); ++g) {}
        assert(g < 4 && nz.bin[b].columns == 96);
        assert(fabs(nz.bin[b].mean - level[g]) < 0.05);
        assert(fabs(nz.bin[b].var_diff - spread[g] * (spread[g] + 1) / 3.0) < 0.05 * spread[g] * (spread[g] + 1) / 3.0 + 0.02);
        assert(fabs(nz.bin[b].var_total - nz.bin[b].var_diff) < 0.1 * nz.bin[b].var_diff + 0.02);
        seen++;
    }
    assert(seen == 4 && nz.fit_ok && nz.fit_bins == 4 && nz.saturated_columns == 0);
    assert(nz.slope > 0.025 && nz.slope < 0.04 && fabs(nz.read_var) < 1.0);
    assert(fabs(nz.tap_mean[0] - 87.5) < 0.1 && fabs(nz.tap_mean[0] - nz.tap_mean[2]) < 0.1);

    for (y = 0; y < H; ++y) img[y * W + 5] = y & 1 ? 255 : 250;       /* 一列饱和：不参与 */
    for (y = 0; y < H; ++y) for (x = 96; x < 192; ++x) img[y * W + x] = (uint8_t)(40 + y / 100);   /* 慢漂：差分方差几乎为 0 */
    assert(sg_pgm_noise(img, W, H, &nz) == 0 && nz.saturated_columns == 1);
    for (b = 0; b < SG_NOISE_BINS; ++b)
        if (nz.bin[b].lo <= 47 && nz.bin[b].hi > 47) assert(nz.bin[b].var_diff < 0.01 && nz.bin[b].var_total > 10.0);
    memset(img, 7, sizeof img);                                         /* 只有一种亮度：不拟合 */
    assert(sg_pgm_noise(img, W, H, &nz) == 0 && !nz.fit_ok && nz.fit_bins == 1);
    assert(sg_pgm_noise(img, W, 1, &nz) == -EINVAL);
    puts("pgm noise ok");
}

/* 线轮廓和对焦指标：造一条已知宽度的边缘，看量出来的 10%–90% 宽度；糊一点锐度就该下降。 */
static void profile_image(uint8_t *img, uint32_t w, uint32_t h, double ramp_px)
{
    uint32_t x, y;

    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        double t = ((double)x - 1000.0) / ramp_px + 0.5;      /* 在 x = 1000 附近从 40 线性升到 200，全程 ramp_px 像素 */
        double v = t <= 0 ? 40 : t >= 1 ? 200 : 40 + 160 * t;
        img[y * w + x] = (uint8_t)(v + 0.5 + (double)(test_rng() % 3) - 1.0);
    }
}

static void test_pgm_profile(void)
{
    enum { W = 2048, H = 400 };
    static uint8_t img[W * H];
    static struct sg_pgm_profile sharp, soft;
    uint32_t b;

    profile_image(img, W, H, 2.5);                 /* 线性斜坡全程 2.5 像素 → 10%–90% = 2.0 像素 */
    assert(sg_pgm_profile(img, W, H, 64, &sharp) == 0 && sharp.bins == 64);
    assert(sharp.edge_found && sharp.edge_x >= 998 && sharp.edge_x <= 1002);
    assert(fabs(sharp.edge_width_px - 2.0) < 0.35 && fabs(sharp.edge_contrast - 160.0) < 2.0);
    assert(fabs(sharp.contrast - 160.0) < 2.0);
    assert(sharp.mean[0] >= 395 && sharp.mean[0] <= 405 && sharp.mean[63] >= 1995 && sharp.mean[63] <= 2005);
    b = 1000 / (W / 64);                                              /* 边缘所在的那一段：最小 ≈ 40，最大 ≈ 200 */
    assert(sharp.min[b] < 420 && sharp.max[b] > 1980 && sharp.min[b] <= sharp.mean[b] && sharp.mean[b] <= sharp.max[b]);
    assert(sharp.seg_sharpness[3] > 10 * sharp.seg_sharpness[0]);     /* 边缘在第 4 段（x = 1000 / 256） */

    profile_image(img, W, H, 10.0);                /* 糊成 10 像素 → 10%–90% = 8 像素，锐度下降 */
    assert(sg_pgm_profile(img, W, H, 64, &soft) == 0 && soft.edge_found);
    assert(fabs(soft.edge_width_px - 8.0) < 0.6 && soft.sharpness < 0.7 * sharp.sharpness);

    memset(img, 90, sizeof img);                   /* 没有边缘 */
    assert(sg_pgm_profile(img, W, H, 64, &soft) == 0 && !soft.edge_found && soft.sharpness < 0.01);
    assert(sg_pgm_profile(img, W, H, 7, &soft) == -EINVAL && sg_pgm_profile(img, W, H, 1025, &soft) == -EINVAL);
    assert(sg_pgm_profile(img, 100, H, 8, &soft) == -EINVAL && sg_pgm_profile(img, W, 0, 64, &soft) == -EINVAL);
    puts("pgm profile ok");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_view_stats(); test_candidates(); test_linecheck(); test_pipeline(); test_cam_serial(); test_cardxml_url(); test_cam_manifest(); test_cam_set(); test_pgmcheck();
    test_stats_row(); test_screen(); test_pgm_noise(); test_pgm_profile();
    puts("unit: ALL PASS");
    return 0;
}
