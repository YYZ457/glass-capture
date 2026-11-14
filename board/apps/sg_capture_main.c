/* SPDX-License-Identifier: MIT
 *
 * sg3000-capture —— 单帧采集程序。
 *
 * 用法（前两条与 legacy 完全一致，中控台依赖这个命令行格式）：
 *   sg3000-capture preflight [--device PATH]
 *   sg3000-capture capture --output FILE.pgm [--device PATH] [--timeout-ms N] [--send IP[:PORT]]
 *   sg3000-capture send FILE.pgm --to IP[:PORT]     把一张图经网口发给电脑（rx_tcp.py）
 *   sg3000-capture version
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
 *
 * 注意：下面打印的每一行文字都要和 legacy 保持一字不差。
 * 中控台（host/console/transport.py）用正则表达式从这些文字里读结果。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sg_io_linux.h"
#include "sg_acq.h"
#include "pgm_write.h"
#include "tx_tcp.h"
#include "cam_serial.h"
#include "cam_xml.h"
#include "cam_set.h"
#include "sg_pgmcheck.h"
#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

#define VERSION_TEXT     "sg3000-capture 1.0 ABI 1"
#define SEND_TIMEOUT_MS  10000u

/* Ctrl+C 处理函数只做一件事：把标志置 1。真正的中止由协议层在检查点读到标志后完成。 */
static volatile sig_atomic_t interrupted;

static void interrupt_handler(int number)
{
    (void)number;
    interrupted = 1;
}

static void usage(void)
{
    puts("Usage: sg3000-capture preflight [--device PATH]\n"
         "       sg3000-capture capture --output PATH [--device PATH] [--timeout-ms 1..60000] [--control-timeout-ms 1..2000] [--send IP[:PORT]]\n"
         "       sg3000-capture send FILE.pgm --to IP[:PORT]\n"
         "       sg3000-capture camera-probe [--device PATH] [--port 0|1] [--baud N]\n"
         "       sg3000-capture camera-xml OUTDIR [--entry N] [--device PATH] [--port 0|1] [--baud N]\n"
         "       sg3000-capture camera-read ADDR [LEN] [--device PATH] [--port 0|1] [--baud N]\n"
         "       sg3000-capture camera-regs NAME@ADDR:LEN ... [--device PATH] [--port 0|1] [--baud N]\n"
         "       sg3000-capture camera-set NAME VALUE [--device PATH] [--baud N]   (runtime only, never UserSetSave)\n"
         "         ClConfiguration 0|3, DeviceTapGeometry 2|3 (Base only), TestPattern 0|1|2|3|5,\n"
         "         ExposureTime <us>, DigitalGain <0.10..15.99>, AnalogGain 0..4 (=1.25/1.75/2/3/4x), LineRate <Hz>\n"
         "         other camera commands are read-only; all need insmod sg3000_rx.ko cam_serial=1, default baud 115200\n"
         "       sg3000-capture pgm-stats FILE.pgm [--thumb OUT.pgm] [--thumb-factor 16]   (no card access)\n"
         "       sg3000-capture card-set NAME VALUE [--device PATH]\n"
         "         NAME: CameraWidth CameraHeight DeviceTapGeometry ClConfiguration CameraScanType\n"
         "       sg3000-capture version\n"
         "Default device: /dev/sg3000_rx_0000:01:00.0\n"
         "Capture explicitly enables the experimental port-0 acquisition sequence.");
}

/* 打印预检的五项值（preflight 和 capture 两种模式共用这一行）。 */
static void print_preflight(const struct sg_capture_preflight *p)
{
    printf("DDRInitialStatus=%u, CameraLinkPixelClocks=0x%08x, CL=%u, Color=%u, Depth=%u\n",
           p->ddr_status, p->pixel_clocks, p->cl_configuration,
           p->camera_color, p->camera_depth);
    if (p->valid_mask == 31 && sg_preflight_required_clocks(p->cl_configuration) &&
        sg_preflight_missing_clocks(p)) {
        fprintf(stderr, "Missing pixel clocks 0x%x for CL=%u: %s\n", sg_preflight_missing_clocks(p),
                p->cl_configuration,
                p->cl_configuration == SG_CL_EIGHTY_BIT && !(sg_preflight_missing_clocks(p) & 1u)
                ? "EightyBit needs BOTH Camera Link cables; clocks 2/3 are on the 2nd cable"
                : "no clock on the 1st Camera Link cable");
    }
}

/* 解析 --timeout-ms 的值：必须是 1..60000 的整数。成功返回 0。 */
static int parse_timeout(const char *text, uint32_t *out)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value == 0 || value > 60000) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

/* 经网口发一块图给电脑。成功打印一行 "Sent frame: ..."。 */
static int send_frame(const struct sg_capture_frame *frame, const char *target)
{
    struct tx_tcp tcp;
    char ip[16];
    uint16_t port;
    int ret;

    ret = tx_tcp_parse_target(target, ip, &port);
    if (ret) {
        return ret;
    }
    ret = tx_tcp_connect(&tcp, ip, port, 3000);
    if (ret) {
        return ret;
    }
    ret = tx_tcp_send_frame(&tcp, frame, SEND_TIMEOUT_MS);
    tx_tcp_close(&tcp);
    if (!ret) {
        printf("Sent frame: %u x %u, %zu bytes to %s:%u\n",
               frame->width, frame->height, frame->length, ip, port);
    }
    return ret;
}

/* sg3000-capture send FILE.pgm --to IP[:PORT]：不碰采集卡，只读文件、发网口。 */
static int send_main(int argc, char **argv)
{
    struct sg_capture_frame frame;
    int ret;

    if (argc != 5 || strcmp(argv[3], "--to")) {
        usage();
        return 2;
    }
    ret = pgm_read(argv[2], &frame, SG_CAPTURE_MAX_BYTES);
    if (!ret) {
        ret = send_frame(&frame, argv[4]);
        sg_capture_frame_free(&frame);
    }
    if (ret) {
        fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    }
    return ret ? 1 : 0;
}

/* ==== 相机命令（GenCP over Camera Link 串口，docs/interfaces/I7）==================
 * 都需要新驱动且 insmod 时带 cam_serial=1（旧驱动会报 Invalid argument (-22)）。
 * 全部只读：只发 GenCP ReadMem，不写相机。 */

#define CAMERA_DEFAULT_BAUD 115200ul   /* 2026-11-14 实板：115200 读通；9600 超时，其余 -71 */
#define CAMERA_TIMEOUT_MS   1000u
#define CAMERA_RETRIES      3u

struct camera_ctx {
    struct sg_linux_dev dev;
    struct sg_capture_session session;
    struct sg_capture_io io;
    struct cam_serial ser;
    int opened;
};

/* 解析 --device / --port / --baud。认识就吃掉参数返回 1，不认识返回 0，值不对返回 -1。 */
static int camera_common_arg(int argc, char **argv, int *i, const char **device,
                             unsigned *port, unsigned long *baud)
{
    char *end;

    if (*i + 1 >= argc) return 0;
    if (!strcmp(argv[*i], "--device")) { *device = argv[++*i]; return 1; }
    if (!strcmp(argv[*i], "--port")) {
        unsigned long v = strtoul(argv[++*i], &end, 10);
        if (*end || v > 1) return -1;
        *port = (unsigned)v;
        return 1;
    }
    if (!strcmp(argv[*i], "--baud")) {
        *baud = strtoul(argv[++*i], &end, 10);
        return *end ? -1 : 1;
    }
    return 0;
}

static void camera_close(struct camera_ctx *x)
{
    if (x->opened) sg_linux_close(&x->dev);
    x->opened = 0;
}

/* 打开设备、（port 1 时）确认是双路卡、打开串口、设波特率。 */
static int camera_open(struct camera_ctx *x, const char *device, unsigned port, unsigned long baud)
{
    uint32_t channels;
    int ret;

    memset(x, 0, sizeof *x);
    ret = sg_linux_open(&x->dev, device, &interrupted);
    if (ret) return ret;
    x->opened = 1;
    sg_linux_io(&x->dev, &x->io);
    ret = sg_capture_session_init(&x->session, &x->io);
    if (ret) return ret;
    if (port != 0) {
        /* 2026-11-14 实板：单路卡上碰 port 1 的寄存器 → nwl-pcie Slave error，整板要重启。
         * 先问卡有几路（VisionKit 同样读这个：低字节 '1' = 2 路）。 */
        ret = sg_mbx_read(&x->session, SG_L_CAMERA_ENUM, &channels, CAMERA_TIMEOUT_MS, 0);
        if (ret) return ret;
        if ((channels & 0xffu) != 0x31u) {
            fprintf(stderr, "Refused: --port 1 needs a 2-channel card, but camera enum = 0x%08x "
                    "(1 channel). Port 1 registers do not exist and touching them crashes the PCIe bus.\n",
                    channels);
            return -ENODEV;
        }
    }
    ret = cam_serial_open(&x->ser, &x->session, port);
    if (!ret) ret = cam_serial_set_baud(&x->ser, (uint32_t)baud);
    return ret;
}

static void print_gencp_string(const char *label, const unsigned char *b)
{
    unsigned i;

    printf("%s=", label);
    for (i = 0; i < CAM_BRM_STRING_LEN && b[i]; ++i) putchar(b[i] >= 0x20 && b[i] < 0x7f ? b[i] : '?');
    putchar('\n');
}

/* 把相机字符串变成能当文件名的样子：只留字母数字和 - _ . */
static void safe_name(const unsigned char *b, char *out, size_t cap)
{
    size_t i, n = 0;

    for (i = 0; i < CAM_BRM_STRING_LEN && b[i] && n + 1 < cap; ++i) {
        unsigned char ch = b[i];
        out[n++] = (char)((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '.' ? ch : '_');
    }
    if (n == 0 && cap > 6) { memcpy(out, "camera", 6); n = 6; }
    out[n] = 0;
}

static int install_interrupt(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ? -errno : 0;
}

static void print_manifest(const struct cam_manifest_entry *e, unsigned i)
{
    unsigned k;

    printf("CameraManifest[%u] file=%u.%u.%u type=%s addr=0x%llx size=%llu sha1=", i,
           e->file_version >> 24, (e->file_version >> 16) & 0xffu, e->file_version & 0xffffu,
           cam_manifest_ext(e) + 1, (unsigned long long)e->address, (unsigned long long)e->size);
    if (cam_manifest_has_sha1(e)) for (k = 0; k < sizeof e->sha1; ++k) printf("%02x", e->sha1[k]);
    else printf("none");
    putchar('\n');
}

/* sg3000-capture camera-probe：读 GenCP 标准设备信息（BRM）和 XML 清单表，只读。 */
static int camera_probe_main(int argc, char **argv)
{
    static const struct { const char *label; uint32_t addr; } strings[] = {
        { "CameraManufacturer", CAM_BRM_MANUFACTURER }, { "CameraModel", CAM_BRM_MODEL },
        { "CameraFamily", CAM_BRM_FAMILY }, { "CameraDeviceVersion", CAM_BRM_DEVICE_VERSION },
        { "CameraManufacturerInfo", CAM_BRM_MANUFACTURER_INFO }, { "CameraSerialNumber", CAM_BRM_SERIAL },
        { "CameraUserName", CAM_BRM_USER_NAME },
    };
    const char *device = SG_DEFAULT_DEVICE;
    unsigned port = 0, i, count = 0;
    unsigned long baud = CAMERA_DEFAULT_BAUD;
    unsigned char b[CAM_BRM_STRING_LEN];
    struct cam_manifest_entry entries[CAM_MANIFEST_MAX_ENTRIES];
    static struct camera_ctx x;
    uint64_t table = 0;
    int a, ret;

    for (a = 2; a < argc; ++a) {
        int r = camera_common_arg(argc, argv, &a, &device, &port, &baud);
        if (r <= 0) { usage(); return 2; }
    }
    ret = camera_open(&x, device, port, baud);
    if (!ret) {
        printf("CameraProbe port=%u baud=%lu\n", port, baud);
        for (i = 0; !ret && i < sizeof strings / sizeof strings[0]; ++i) {
            int r = cam_read_mem(&x.ser, strings[i].addr, b, sizeof b, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
            if (!r) print_gencp_string(strings[i].label, b);
            else if (i < 2) ret = r;        /* 厂商和型号必须读得出；其余只是参考 */
            else printf("%s unreadable: %s (%d)\n", strings[i].label, strerror(-r), r);
        }
    }
    if (!ret) ret = cam_read_mem(&x.ser, CAM_BRM_GENCP_VERSION, b, 4, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (!ret) printf("CameraGenCPVersion=%u.%u\n", (unsigned)(b[0] << 8 | b[1]), (unsigned)(b[2] << 8 | b[3]));
    if (!ret) ret = cam_read_mem(&x.ser, CAM_BRM_CAPABILITY, b, 16, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (!ret) printf("CameraCapability=0x%016llx MaxResponseMs=%u\n", (unsigned long long)cam_be64(b),
                     (unsigned)((uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11]));
    if (!ret) ret = cam_read_mem(&x.ser, CAM_BRM_SBRM_ADDRESS, b, 8, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (!ret) printf("CameraSBRMAddress=0x%llx\n", (unsigned long long)cam_be64(b));
    if (!ret) ret = cam_manifest_read(&x.ser, &table, entries, CAM_MANIFEST_MAX_ENTRIES, &count, CAMERA_TIMEOUT_MS);
    if (!ret) {
        printf("CameraManifestTable=0x%llx entries=%u\n", (unsigned long long)table, count);
        for (i = 0; i < count && i < CAM_MANIFEST_MAX_ENTRIES; ++i) print_manifest(&entries[i], i);
    }
    camera_close(&x);
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

struct xml_progress { uint64_t next; };

static void xml_progress(uint64_t done, uint64_t total, void *ctx)
{
    struct xml_progress *p = ctx;

    if (done >= p->next || done == total) {
        printf("CameraXML progress %llu/%llu\n", (unsigned long long)done, (unsigned long long)total);
        fflush(stdout);
        p->next = done + 16384u;
    }
}

/* sg3000-capture camera-xml OUTDIR：从相机读出它自己的 GenICam XML（.zip 或 .xml），只读。 */
static int camera_xml_main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE, *dir = NULL;
    unsigned port = 0, entry = 0, count = 0, k;
    unsigned long baud = CAMERA_DEFAULT_BAUD;
    unsigned char model[CAM_BRM_STRING_LEN];
    struct cam_manifest_entry entries[CAM_MANIFEST_MAX_ENTRIES], *e;
    struct xml_progress progress = { 0 };
    static struct camera_ctx x;
    char name[CAM_BRM_STRING_LEN + 1], path[4096], tmp[4200];
    unsigned char *data = NULL;
    uint64_t table = 0, t0;
    FILE *f;
    int a, ret;

    for (a = 2; a < argc; ++a) {
        int r = camera_common_arg(argc, argv, &a, &device, &port, &baud);
        char *end;
        if (r < 0) { usage(); return 2; }
        if (r) continue;
        if (!strcmp(argv[a], "--entry") && a + 1 < argc) {
            unsigned long v = strtoul(argv[++a], &end, 10);
            if (*end || v >= CAM_MANIFEST_MAX_ENTRIES) { usage(); return 2; }
            entry = (unsigned)v;
        } else if (!dir && argv[a][0] != '-') dir = argv[a];
        else { usage(); return 2; }
    }
    if (!dir) { usage(); return 2; }
    if ((ret = install_interrupt())) goto out;

    ret = camera_open(&x, device, port, baud);
    if (!ret) ret = cam_read_mem(&x.ser, CAM_BRM_MODEL, model, sizeof model, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (!ret) ret = cam_manifest_read(&x.ser, &table, entries, CAM_MANIFEST_MAX_ENTRIES, &count, CAMERA_TIMEOUT_MS);
    if (ret) goto out;
    printf("CameraManifestTable=0x%llx entries=%u\n", (unsigned long long)table, count);
    for (k = 0; k < count && k < CAM_MANIFEST_MAX_ENTRIES; ++k) print_manifest(&entries[k], k);
    if (entry >= count) {
        fprintf(stderr, "CameraXML FAILED: --entry %u but the camera lists %u entries\n", entry, count);
        ret = -EINVAL;
        goto out;
    }
    e = &entries[entry];
    safe_name(model, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s_%u.%u.%u%s", dir, name, e->file_version >> 24,
             (e->file_version >> 16) & 0xffu, e->file_version & 0xffffu, cam_manifest_ext(e));
    snprintf(tmp, sizeof tmp, "%s.part", path);

    data = malloc((size_t)e->size);
    if (!data) { ret = -ENOMEM; goto out; }
    printf("CameraXML reading entry %u: %llu bytes at 0x%llx (about %llu s at %lu baud)\n", entry,
           (unsigned long long)e->size, (unsigned long long)e->address,
           (unsigned long long)(e->size * 13u / (baud / 10u) + 1u), baud);
    fflush(stdout);
    t0 = x.io.now_ms(x.io.context);
    ret = cam_read_mem(&x.ser, e->address, data, e->size, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, xml_progress, &progress);
    if (ret) goto out;

    f = fopen(tmp, "wb");
    if (!f) { ret = -errno; goto out; }
    if (fwrite(data, 1, (size_t)e->size, f) != (size_t)e->size) ret = -EIO;
    if (fclose(f) && !ret) ret = -EIO;
    if (!ret && rename(tmp, path)) ret = -errno;
    if (ret) { unlink(tmp); goto out; }

    printf("CameraXML saved: %s (%llu bytes, %llu s)\n", path, (unsigned long long)e->size,
           (unsigned long long)((x.io.now_ms(x.io.context) - t0) / 1000u));
    if (cam_manifest_has_sha1(e)) {
        printf("CameraXML SHA1 expected ");
        for (k = 0; k < sizeof e->sha1; ++k) printf("%02x", e->sha1[k]);
        putchar('\n');
    }
    if (!strcmp(cam_manifest_ext(e), ".zip") ? memcmp(data, "PK", 2) : (data[0] != '<' && data[0] != 0xef))
        printf("CameraXML WARNING: content does not look like a %s file (first bytes %02x %02x)\n",
               cam_manifest_ext(e) + 1, data[0], e->size > 1 ? data[1] : 0);
out:
    free(data);
    camera_close(&x);
    if (ret) fprintf(stderr, "CameraXML FAILED: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end;
    unsigned long long v;

    errno = 0;
    v = strtoull(text, &end, 0);
    if (errno || end == text || *end) return -1;
    *out = v;
    return 0;
}

static void hexdump(const unsigned char *b, uint32_t n, uint64_t base)
{
    uint32_t i, j;

    for (i = 0; i < n; i += 16) {
        printf("  %08llx ", (unsigned long long)(base + i));
        for (j = i; j < i + 16; ++j) j < n ? printf(" %02x", b[j]) : printf("   ");
        printf("  ");
        for (j = i; j < i + 16 && j < n; ++j) putchar(b[j] >= 0x20 && b[j] < 0x7f ? b[j] : '.');
        putchar('\n');
    }
}

/* 大端整数（1/2/4/8 字节）。 */
static uint64_t be_value(const unsigned char *b, uint32_t n)
{
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < n; ++i) v = v << 8 | b[i];
    return v;
}

/* sg3000-capture camera-read ADDR [LEN]：读相机任意地址，十六进制打印。只读。 */
static int camera_read_main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    unsigned port = 0;
    unsigned long baud = CAMERA_DEFAULT_BAUD;
    uint64_t addr = 0, len = 4;
    unsigned char b[4096];
    static struct camera_ctx x;
    int a, have_addr = 0, have_len = 0, ret;

    for (a = 2; a < argc; ++a) {
        int r = camera_common_arg(argc, argv, &a, &device, &port, &baud);
        if (r < 0) { usage(); return 2; }
        if (r) continue;
        if (!have_addr && !parse_u64(argv[a], &addr)) have_addr = 1;
        else if (have_addr && !have_len && !parse_u64(argv[a], &len)) have_len = 1;
        else { usage(); return 2; }
    }
    if (!have_addr || len == 0 || len > sizeof b) { usage(); return 2; }
    if ((ret = install_interrupt())) goto out;
    ret = camera_open(&x, device, port, baud);
    if (!ret) ret = cam_read_mem(&x.ser, addr, b, len, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (!ret) {
        printf("CameraRead 0x%llx %llu bytes", (unsigned long long)addr, (unsigned long long)len);
        if (len == 1 || len == 2 || len == 4 || len == 8)
            printf(" = 0x%llx (%llu) big-endian", (unsigned long long)be_value(b, (uint32_t)len),
                   (unsigned long long)be_value(b, (uint32_t)len));
        putchar('\n');
        hexdump(b, (uint32_t)len, addr);
    }
out:
    camera_close(&x);
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

/* sg3000-capture camera-regs NAME@ADDR:LEN ...：按清单读一串相机寄存器（清单由
 * tools/genicam_regs.py 从相机 XML 生成）。某一项失败只记下来，继续读下一项。只读。 */
static int camera_regs_main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    unsigned port = 0, failed = 0, total = 0;
    unsigned long baud = CAMERA_DEFAULT_BAUD;
    static struct camera_ctx x;
    int a, ret;

    /* 先把清单全部检查一遍，格式不对就什么都不碰。 */
    for (a = 2; a < argc; ++a) {
        int r = camera_common_arg(argc, argv, &a, &device, &port, &baud);
        const char *at = strchr(argv[a], '@'), *colon = at ? strchr(at, ':') : NULL;
        char *end;
        unsigned long long v;
        if (r < 0) { usage(); return 2; }
        if (r) continue;
        if (!at || !colon || at == argv[a] || at - argv[a] > 63) { usage(); return 2; }
        errno = 0;
        v = strtoull(at + 1, &end, 0);
        if (errno || end != colon) { usage(); return 2; }
        v = strtoull(colon + 1, &end, 10);
        if (errno || *end || v == 0 || v > 256) { usage(); return 2; }
        total++;
    }
    if (!total) { usage(); return 2; }
    if ((ret = install_interrupt())) goto out;
    ret = camera_open(&x, device, port, baud);
    if (ret) goto out;
    printf("CameraRegs port=%u baud=%lu count=%u\n", port, baud, total);
    for (a = 2; a < argc; ++a) {
        const char *at, *colon;
        char name[64];
        unsigned char b[256];
        uint64_t addr;
        uint32_t len;
        int r;

        if (!strcmp(argv[a], "--device") || !strcmp(argv[a], "--port") || !strcmp(argv[a], "--baud")) { ++a; continue; }
        at = strchr(argv[a], '@');
        colon = strchr(at, ':');
        memcpy(name, argv[a], (size_t)(at - argv[a]));
        name[at - argv[a]] = 0;
        addr = strtoull(at + 1, NULL, 0);
        len = (uint32_t)strtoul(colon + 1, NULL, 10);
        r = cam_read_mem(&x.ser, addr, b, len, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
        if (r == -ECANCELED) { ret = r; break; }
        if (r) {
            printf("CameraReg %s 0x%llx ERROR %s (%d)\n", name, (unsigned long long)addr, strerror(-r), r);
            failed++;
        } else if (len == 1 || len == 2 || len == 4 || len == 8) {
            printf("CameraReg %s 0x%llx = 0x%llx (%llu)\n", name, (unsigned long long)addr,
                   (unsigned long long)be_value(b, len), (unsigned long long)be_value(b, len));
        } else {
            uint32_t k;
            printf("CameraReg %s 0x%llx = \"", name, (unsigned long long)addr);
            for (k = 0; k < len && b[k]; ++k) putchar(b[k] >= 0x20 && b[k] < 0x7f && b[k] != '"' ? b[k] : '?');
            printf("\" hex ");
            for (k = 0; k < len; ++k) printf("%02x", b[k]);
            putchar('\n');
        }
        fflush(stdout);
    }
    printf("CameraRegs done: %u ok, %u failed\n", total - failed, failed);
    if (!ret && failed) ret = -EIO;
out:
    camera_close(&x);
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

/* sg3000-capture camera-set NAME VALUE：改相机的运行时参数（GenCP WriteMem）。
 * 允许的名字、值、锁定条件见 camera_ctrl/cam_set.h（全部来自这台 16K 相机自己的 XML）。
 * 写之前核对型号、读当前状态；写后读回。从不写 UserSetSave：相机断电恢复 UserSet1。 */
static void print_camera_value(const char *name, uint32_t raw)
{
    if (!strcmp(name, "ExposureTime"))
        printf("CameraValue ExposureTime = %.2f us\n", raw / (double)CAM16K_SENSOR_MHZ);
    else if (!strcmp(name, "DigitalGain"))
        printf("CameraValue DigitalGain = %.3f x\n", raw / 256.0);
    else if (!strcmp(name, "LineRate"))
        printf("CameraValue LineRate = %.1f Hz\n", raw ? CAM16K_SENSOR_MHZ * 1e6 / raw : 0.0);
    else if (!strcmp(name, "AnalogGain")) {
        static const char *g[] = { "1.25", "1.75", "2.0", "3.0", "4.0" };
        printf("CameraValue AnalogGain = %s x\n", raw < 5 ? g[raw] : "?");
    }
}

static int camera_set_main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE, *why = NULL;
    unsigned port = 0;
    unsigned long baud = CAMERA_DEFAULT_BAUD;
    unsigned char model[CAM_BRM_STRING_LEN];
    static struct camera_ctx x;
    struct cam_set_state st;
    uint32_t addr, raw = 0, old = 0, back = 0, cl = 0, tap = 0;
    int a, ret;

    if (argc < 4 || !(addr = cam_set_address(argv[2]))) { usage(); return 2; }
    for (a = 4; a < argc; ++a) {
        int r = camera_common_arg(argc, argv, &a, &device, &port, &baud);
        if (r <= 0) { usage(); return 2; }
    }
    memset(&st, 0, sizeof st);
    if (cam_set_check(argv[2], argv[3], &(struct cam_set_state){ 0, CAM16K_MONO8, 0, 1, 0xffffffffu, 1, 0xffffffffu, 0 },
                      &raw, &why) == -EINVAL) {
        usage();   /* 格式就不对（比如 TestPattern abc）：不碰硬件 */
        return 2;
    }
    if ((ret = install_interrupt())) goto out;
    ret = camera_open(&x, device, port, baud);
    if (!ret) ret = cam_read_mem(&x.ser, CAM_BRM_MODEL, model, sizeof model, CAMERA_TIMEOUT_MS, CAMERA_RETRIES, NULL, NULL);
    if (ret) goto out;
    model[sizeof model - 1] = 0;
    if (strcmp((const char *)model, CAM16K_MODEL)) {
        fprintf(stderr, "Refused: camera model is \"%s\", register addresses are only verified for %s\n",
                (const char *)model, CAM16K_MODEL);
        ret = -ENODEV;
        goto out;
    }
    if ((ret = cam_gencp_read32(&x.ser, CAM16K_CL_CONFIG, &st.cl_config, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_PIXEL_FORMAT, &st.pixel_format, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_BINNING_V, &st.binning_v, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_EXPOSURE_MIN_CLK, &st.exposure_min_clk, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_EXPOSURE_MAX_CLK, &st.exposure_max_clk, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_LINE_PERIOD_MIN, &st.line_period_min_clk, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_LINE_PERIOD_MAX, &st.line_period_max_clk, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_LINE_TRIGGER_MODE, &st.line_trigger_mode, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, addr, &old, CAMERA_TIMEOUT_MS)))
        goto out;
    ret = cam_set_check(argv[2], argv[3], &st, &raw, &why);
    if (ret) {
        fprintf(stderr, "Refused: %s = %s (%s). Camera now: ClConfiguration=%u PixelFormat=0x%08x BinningV=%u "
                "Exposure range %.2f..%.2f us, LineRate range %.1f..%.1f Hz, LineTriggerMode=%u\n",
                argv[2], argv[3], why ? why : "not allowed",
                st.cl_config, st.pixel_format, st.binning_v,
                st.exposure_min_clk / (double)CAM16K_SENSOR_MHZ, st.exposure_max_clk / (double)CAM16K_SENSOR_MHZ,
                st.line_period_max_clk ? CAM16K_SENSOR_MHZ * 1e6 / st.line_period_max_clk : 0.0,
                st.line_period_min_clk ? CAM16K_SENSOR_MHZ * 1e6 / st.line_period_min_clk : 0.0,
                st.line_trigger_mode);
        goto out;
    }
    if ((ret = cam_gencp_write32(&x.ser, addr, raw, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, addr, &back, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_CL_CONFIG, &cl, CAMERA_TIMEOUT_MS)) ||
        (ret = cam_gencp_read32(&x.ser, CAM16K_TAP_GEOMETRY, &tap, CAMERA_TIMEOUT_MS)))
        goto out;
    printf("CameraSet %s 0x%08x: %u -> %u, readback %u\n", argv[2], addr, old, raw, back);
    print_camera_value(argv[2], back);
    printf("CameraState ClConfiguration=%u (0 Base / 3 EightyBit) DeviceTapGeometry=%u PixelFormat=0x%08x "
           "(not saved: camera power cycle restores UserSet1)\n", cl, tap, st.pixel_format);
    if (back != raw) {
        fprintf(stderr, "Camera did not keep the value\n");
        ret = -EIO;
    }
out:
    camera_close(&x);
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

/* sg3000-capture pgm-stats FILE [--thumb OUT.pgm] [--thumb-factor N]：
 * 在板上检查一张图（亮度、8 个位、测试图渐变公式），可选存一张缩略图（够小，能经串口取回）。
 * 不碰采集卡。 */
static int pgm_stats_main(int argc, char **argv)
{
    const char *thumb = NULL;
    unsigned long factor = 16;
    struct sg_capture_frame frame, small;
    struct sg_pgm_check c;
    char *end;
    int a, k, ret;

    if (argc < 3) { usage(); return 2; }
    for (a = 3; a < argc; ++a) {
        if (!strcmp(argv[a], "--thumb") && a + 1 < argc) thumb = argv[++a];
        else if (!strcmp(argv[a], "--thumb-factor") && a + 1 < argc) {
            factor = strtoul(argv[++a], &end, 10);
            if (*end || factor < 1 || factor > 256) { usage(); return 2; }
        } else { usage(); return 2; }
    }
    ret = pgm_read(argv[2], &frame, SG_CAPTURE_MAX_BYTES);
    if (ret) goto out;
    ret = sg_pgm_check(frame.data, frame.width, frame.height, &c);
    if (!ret) {
        printf("PGMStats %s %ux%u min=%u max=%u mean=%.3f p1=%u p50=%u p99=%u saturated=%llu\n", argv[2],
               c.width, c.height, c.min, c.max, c.mean, c.p1, c.p50, c.p99, (unsigned long long)c.saturated);
        printf("PGMBits OR=0x%02x missing=", c.or_all);
        for (k = 0, a = 0; k < 8; ++k) if (!(c.or_all >> k & 1)) printf("%s%d", a++ ? "," : "", k);
        printf("%s tap0=0x%02x tap1=0x%02x tap2=0x%02x\n", a ? "" : "none", c.or_tap[0], c.or_tap[1], c.or_tap[2]);
        printf("PGMRamp");
        for (k = 0; k < SG_RAMP_COUNT; ++k) {
            if (c.ramp_mismatch[k]) printf(" %s=%llu_mismatched", sg_ramp_name(k), (unsigned long long)c.ramp_mismatch[k]);
            else printf(" %s=MATCH", sg_ramp_name(k));
        }
        printf("\nPGMFlat all0=%s all255=%s\n", c.all_zero ? "yes" : "no", c.all_255 ? "yes" : "no");
    }
    if (!ret && thumb) {
        uint32_t tw = 0, th = 0;
        memset(&small, 0, sizeof small);
        small.data = malloc((size_t)(frame.width / factor) * (frame.height / factor) + 1);
        if (!small.data) ret = -ENOMEM;
        if (!ret) ret = sg_pgm_thumb(frame.data, frame.width, frame.height, (uint32_t)factor, small.data, &tw, &th);
        if (!ret) {
            small.width = tw;
            small.height = th;
            small.length = (size_t)tw * th;
            ret = pgm_write(thumb, &small);
            if (!ret) printf("PGMThumb %s %ux%u (1/%lu)\n", thumb, tw, th, factor);
        }
        free(small.data);
    }
    sg_capture_frame_free(&frame);
out:
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

/* sg3000-capture card-set NAME VALUE：写一个卡参数（地址和值域来自卡的 XML）。
 * 只在采集已停止（SG_ACQ_STATE = "STOP"）时写；写前读一次、写后读回核对。 */
static const struct { const char *name; uint32_t addr; } CARD_SET[] = {
    { "CameraWidth", SG_L_CAMERA_WIDTH }, { "CameraHeight", SG_L_CAMERA_HEIGHT },
    { "DeviceTapGeometry", SG_L_TAP_GEOMETRY }, { "ClConfiguration", SG_L_CL_CONFIG },
    { "CameraScanType", SG_L_SCAN_TYPE },
};

static int card_set_main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    struct sg_capture_session session;
    struct sg_capture_options options;
    struct sg_capture_io io;
    struct sg_linux_dev dev;
    uint32_t addr = 0, value, old, back, state, other;
    unsigned long parsed;
    char *end;
    size_t k;
    int i, ret;

    if (argc < 4) { usage(); return 2; }
    for (k = 0; k < sizeof CARD_SET / sizeof CARD_SET[0]; ++k)
        if (!strcmp(argv[2], CARD_SET[k].name)) addr = CARD_SET[k].addr;
    errno = 0;
    parsed = strtoul(argv[3], &end, 0);
    if (!addr || errno || *end || parsed > 0xffffffffUL) { usage(); return 2; }
    value = (uint32_t)parsed;
    for (i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else { usage(); return 2; }
    }
    if (!sg_mbx_write_allowed(addr, value)) {
        fprintf(stderr, "Refused: %s = %u is outside the range allowed by the card XML\n", argv[2], value);
        return 2;
    }

    sg_capture_default_options(&options);
    ret = sg_linux_open(&dev, device, &interrupted);
    if (ret) goto out;
    sg_linux_io(&dev, &io);
    if ((ret = sg_capture_session_init(&session, &io)) ||
        (ret = sg_session_rd(&session, SG_ACQ_STATE, &state)))
        goto close;
    if (state != SG_ACQ_STATE_STOP) {
        fprintf(stderr, "Refused: acquisition is not stopped (SG_ACQ_STATE 0x%08x)\n", state);
        ret = -EBUSY;
        goto close;
    }
    /* 宽 × 高不能超过单块上限（Mono8 一字节一像素）*/
    if (addr == SG_L_CAMERA_WIDTH || addr == SG_L_CAMERA_HEIGHT) {
        ret = sg_mbx_read(&session, addr == SG_L_CAMERA_WIDTH ? SG_L_CAMERA_HEIGHT : SG_L_CAMERA_WIDTH,
                          &other, options.control_timeout_ms, 0);
        if (ret) goto close;
        if ((uint64_t)value * other > SG_CAPTURE_MAX_BYTES) {
            fprintf(stderr, "Refused: %u x %u exceeds the %u-byte block limit\n", value, other, SG_CAPTURE_MAX_BYTES);
            ret = -EFBIG;
            goto close;
        }
    }
    if ((ret = sg_mbx_read(&session, addr, &old, options.control_timeout_ms, 0)) ||
        (ret = sg_mbx_write_value(&session, addr, value, options.control_timeout_ms)) ||
        (ret = sg_mbx_read(&session, addr, &back, options.control_timeout_ms, 0)))
        goto close;
    printf("CardSet %s 0x%08x: %u -> %u, readback %u\n", argv[2], addr, old, value, back);
    if (back != value) {
        fprintf(stderr, "Card did not keep the value\n");
        ret = -EIO;
    }
close:
    sg_linux_close(&dev);
out:
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    const char *output = NULL, *send_to = NULL;
    struct sg_capture_options options;
    struct sg_capture_session session;
    struct sg_capture_result result;
    struct sg_capture_preflight preflight;
    struct sg_capture_io io;
    struct sg_linux_dev dev;
    struct sigaction action;
    struct stat st;
    int capture, ret = 0, i;

    sg_capture_default_options(&options);
    memset(&preflight, 0, sizeof(preflight));

    /* ---- 解析命令行 ---------------------------------------------------- */
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
        usage();
        return argc < 2;
    }
    if (!strcmp(argv[1], "version")) {
        puts(VERSION_TEXT);
        return 0;
    }
    if (!strcmp(argv[1], "send")) {
        return send_main(argc, argv);
    }
    if (!strcmp(argv[1], "camera-probe")) {
        return camera_probe_main(argc, argv);
    }
    if (!strcmp(argv[1], "camera-xml")) {
        return camera_xml_main(argc, argv);
    }
    if (!strcmp(argv[1], "camera-read")) {
        return camera_read_main(argc, argv);
    }
    if (!strcmp(argv[1], "camera-regs")) {
        return camera_regs_main(argc, argv);
    }
    if (!strcmp(argv[1], "camera-set")) {
        return camera_set_main(argc, argv);
    }
    if (!strcmp(argv[1], "pgm-stats")) {
        return pgm_stats_main(argc, argv);
    }
    if (!strcmp(argv[1], "card-set")) {
        return card_set_main(argc, argv);
    }
    capture = !strcmp(argv[1], "capture");
    if (!capture && strcmp(argv[1], "preflight")) {
        usage();
        return 2;
    }
    for (i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device = argv[++i];
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc && capture && !output) {
            output = argv[++i];
        } else if (!strcmp(argv[i], "--send") && i + 1 < argc && capture) {
            send_to = argv[++i];
        } else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc && capture) {
            if (parse_timeout(argv[++i], &options.frame_timeout_ms)) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--control-timeout-ms") && i + 1 < argc && capture) {
            /* 诊断用：邮箱命令（开始/停止）的超时，默认 1000，上限 2000 */
            if (parse_timeout(argv[++i], &options.control_timeout_ms) || options.control_timeout_ms > 2000) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (capture && (!output || !*output)) {
        usage();
        return 2;
    }

    /* 输出文件已存在就拒绝，绝不覆盖。 */
    if (output) {
        if (!lstat(output, &st)) {
            fprintf(stderr, "Output already exists; refusing overwrite\n");
            return 1;
        }
        if (errno != ENOENT) {
            perror("output path");
            return 1;
        }
    }

    /* ---- 注册 Ctrl+C / kill 的处理 ------------------------------------- */
    memset(&action, 0, sizeof(action));
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction");
        return 1;
    }

    /* ---- 打开设备，组装回调表和会话 ------------------------------------ */
    ret = sg_linux_open(&dev, device, &interrupted);
    if (ret) {
        goto done;
    }
    printf("SG3000 %04x:%02x:%02x.%u, C2H0 AXI MM, ABI %u\n", dev.info.domain,
           dev.info.bus, dev.info.devfn >> 3, dev.info.devfn & 7u, dev.info.abi);

    sg_linux_io(&dev, &io);
    ret = sg_capture_session_init(&session, &io);
    if (ret) {
        goto done;
    }

    /* ---- 干活 ---------------------------------------------------------- */
    if (!capture) {
        /* preflight：只读体检，不启动采集。 */
        ret = sg_capture_preflight(&session, options.control_timeout_ms, &preflight);
        print_preflight(&preflight);
        if (!ret) {
            puts("Preflight passed. Acquisition and DMA were not started.");
        }
    } else {
        /* capture：开始 → 取一块 → 停止，再存成 PGM。 */
        ret = sg_capture_once(&session, &options, &result);
        print_preflight(&result.preflight);
        if (result.stop_error) {
            fprintf(stderr, "AcquisitionStop cleanup failed: %s (%d)\n",
                    strerror(-result.stop_error), result.stop_error);
        }
        /* 诊断：失败卡在哪一步、最近一个块头（2026-11-14 16K 宽度 -110 就靠这几行定位）*/
        if (result.primary_error && result.primary_stage) {
            fprintf(stderr, "FailedStage: %s\n", result.primary_stage);
        }
        if (result.stop_error && result.stop_stage) {
            fprintf(stderr, "StopFailedStage: %s\n", result.stop_stage);
        }
        if ((result.primary_error || result.stop_error) && session.last_hdr.valid) {
            fprintf(stderr, "LastHeader block=%u seq=0x%08x width=%u lines=%u max_lines=%u "
                    "payload=%u dest_offset=%u src=0x%08x\n",
                    session.last_hdr.block_id, session.last_hdr.seq, session.last_hdr.width,
                    session.last_hdr.lines, session.last_hdr.max_lines, session.last_hdr.payload_len,
                    session.last_hdr.dest_offset, session.last_hdr.src_addr);
        } else if (result.primary_error || result.stop_error) {
            fprintf(stderr, "LastHeader none (no block header was read)\n");
        }
        if (!ret) {
            ret = pgm_write(output, &result.frame);
            if (!ret) {
                printf("Saved complete Mono8 frame: %u x %u, %zu bytes, block %u, %u packets: %s\n",
                       result.frame.width, result.frame.height, result.frame.length,
                       result.frame.block_id, result.frame.packets, output);
                printf("DMAStats chunk=%u transfers=%llu bytes=%llu dma_ms=%llu MBps=%.1f wait_ms=%llu\n",
                       session.dma_chunk, (unsigned long long)session.stats.dma_calls,
                       (unsigned long long)session.stats.dma_bytes, (unsigned long long)session.stats.dma_ms,
                       session.stats.dma_ms ? session.stats.dma_bytes / 1000.0 / session.stats.dma_ms : 0.0,
                       (unsigned long long)session.stats.wait_ms);
                fflush(stdout);
                if (send_to) {
                    ret = send_frame(&result.frame, send_to);   /* 图已经存好；发送失败只影响退出码 */
                }
            }
        }
        sg_capture_frame_free(&result.frame);
    }

done:
    sg_linux_close(&dev);   /* dev 没打开成功时里面的 fd 也是 -1 或已关闭，安全 */
    if (ret) {
        fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    }
    return ret ? 1 : 0;
}
