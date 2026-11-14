/* SPDX-License-Identifier: MIT
 *
 * sg_status —— 只读体检程序（零风险：不启动采集、不写任何控制寄存器）。
 *
 * 用法：sg_status [--device PATH] [--card-xml DIR]
 * 输出：
 *   1. 驱动信息（SG_IOC_INFO）：PCIe 位置、ABI、引擎 ID、对齐要求、poisoned
 *   2. 预检结果：DDR 状态、时钟寄存器（逐位打印）、CL 配置、彩色、位深
 *   3. SG_HDR_READY、SG_ACQ_STATE 的当前值，以及 VisionKit 枚举相机时读的 0x10540010
 *   4. （--card-xml）从卡存储读出卡自己的 GenICam XML，存到 DIR，见 sg_cardxml.h
 * 退出码：0 一切正常；1 预检不通过；2 打不开设备；3 读卡 XML 失败
 *
 * 组装：sg_io_linux（打开设备）→ sg_session → sg_preflight
 * 没有 legacy 来源；可参考 legacy sg3000_capture.c 的 "preflight" 子命令。
 *
 * 这个程序里只有"读"：
 *   - 邮箱读（预检五项 + 0x10540010；--card-xml 时还有 URL 和 XML 所在的地址）
 *   - 直接读两个 BAR0 寄存器（SG_HDR_READY、SG_ACQ_STATE）
 * 没有任何写，也没有启动 DMA。
 *
 * 除参数错误外，所有信息都输出到 stdout；"打不开设备"这类失败信息输出到 stderr。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>

#include "sg_io_linux.h"
#include "sg_session.h"
#include "sg_preflight.h"
#include "sg_mailbox.h"
#include "sg_cardxml.h"
#include <stdlib.h>
#include <time.h>
#include "sg3000_regs.h"
#include "sg_err.h"

/* 退出码 */
#define EXIT_OK          0
#define EXIT_PREFLIGHT   1
#define EXIT_OPEN_FAIL   2
#define EXIT_CARD_XML    3

static void usage(void)
{
    puts("Usage: sg_status [--device PATH] [--card-xml DIR]\n"
         "Read-only check: driver info, preflight, header-ready and acquisition state.\n"
         "--card-xml DIR  also read the card's own GenICam XML from card storage into DIR.\n"
         "Default device: " SG_DEFAULT_DEVICE);
}

/* ---- 1. 驱动信息 ----------------------------------------------------- */

static void print_driver_info(const struct sg_info *info)
{
    puts("== Driver ==");
    printf("PCIe            %04x:%02x:%02x.%u   (vendor 0x%04x, device 0x%04x)\n",
           info->domain, info->bus, info->devfn >> 3, info->devfn & 7u,
           info->vendor, info->device);
    printf("ABI             %u\n", info->abi);
    printf("engine_id       0x%08x\n", info->engine_id);
    printf("alignment       address %u, length %u, address_bits %u\n",
           info->address_alignment, info->length_granularity, info->address_bits);
    printf("max_transfer    %u bytes\n", info->max_transfer);
    printf("poisoned        %u   (non-zero = driver has isolated the card)\n", info->poisoned);
}

/* ---- 2. 预检 --------------------------------------------------------- */

/* 把一个 32 位数里为 1 的位列出来，例如 0x59 → "0 3 4 6"。 */
static void print_set_bits(uint32_t value)
{
    unsigned bit;
    int any = 0;

    for (bit = 0; bit < 32; ++bit) {
        if (value & (1u << bit)) {
            printf("%s%u", any ? " " : "", bit);
            any = 1;
        }
    }
    if (!any) {
        printf("(none)");
    }
}

static void print_preflight(const struct sg_capture_preflight *p, int ret)
{
    puts("== Preflight (read-only) ==");
    /* valid_mask 的第 N 位为 1，说明第 N 项成功读到了；没读到的项这里照样打印，但值无效。 */
    printf("valid_mask      0x%02x\n", p->valid_mask);
    printf("DDR status      %u   (0 = uninitialized, 2 = calibrated; both normal)\n",
           p->ddr_status);
    printf("pixel clocks    0x%08x   bits set: ", p->pixel_clocks);
    print_set_bits(p->pixel_clocks);
    puts("   (bit0 = Camera Link clock 1; other bits unknown)");
    printf("CL config       %u   (0 = Base)\n", p->cl_configuration);
    printf("camera color    %u   (0 = mono)\n", p->camera_color);
    printf("camera depth    %u   (8 expected)\n", p->camera_depth);

    if (ret) {
        printf("Preflight FAILED: %s (%d)\n", strerror(-ret), ret);
    } else {
        puts("Preflight passed.");
    }
}

/* ---- 3. 两个状态寄存器 ----------------------------------------------- */

static void print_registers(struct sg_capture_session *s, uint32_t timeout_ms)
{
    uint32_t value;
    int ret;

    puts("== Registers ==");

    ret = sg_session_rd(s, SG_HDR_READY, &value);
    if (ret) {
        printf("SG_HDR_READY    read failed: %s (%d)\n", strerror(-ret), ret);
    } else {
        printf("SG_HDR_READY    %u   (non-zero = a data block is waiting)\n", value);
    }

    ret = sg_session_rd(s, SG_ACQ_STATE, &value);
    if (ret) {
        printf("SG_ACQ_STATE    read failed: %s (%d)\n", strerror(-ret), ret);
    } else {
        printf("SG_ACQ_STATE    0x%08x%s\n", value,
               value == SG_ACQ_STATE_STOP ? "   (\"STOP\")" : "");
    }

    /* VisionKit 枚举相机时读的逻辑地址（[逆向]），只读。0x31 = '1'。 */
    ret = sg_mbx_read(s, SG_L_CAMERA_ENUM, &value, timeout_ms, 0);
    if (ret) {
        printf("camera enum     read failed: %s (%d)\n", strerror(-ret), ret);
    } else {
        printf("camera enum     0x%08x   (VisionKit: low byte 0x31 '1' = 2 channels, else 1)\n", value);
    }
}

/* ---- 4. 卡自己的 XML（可选）------------------------------------------ */

static void swap_words(unsigned char *p, size_t n)
{
    size_t i;
    unsigned char t;

    for (i = 0; i + 3 < n; i += 4) {
        t = p[i]; p[i] = p[i + 3]; p[i + 3] = t;
        t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t;
    }
}

/* 诊断：把最近一条邮箱回复原样打出来（十六进制，按 4 字节一组，小端字）。 */
static void print_last_reply(const struct sg_capture_session *s, const char *what)
{
    uint32_t i, n = s->last_reply_len < sizeof s->last_reply ? s->last_reply_len : sizeof s->last_reply;

    printf("CardXML reply %s: %u bytes:", what, s->last_reply_len);
    for (i = 0; i < n; ++i) printf("%s%02x", i % 4 ? "" : " ", s->last_reply[i]);
    printf("\n");
}

/* 读 URL 失败时再试两种读法，各打一行结果和原始回复，帮助判断卡到底回了什么。只读。 */
static void diagnose_url_read(struct sg_capture_session *s, uint32_t timeout_ms)
{
    unsigned char b[4];
    uint32_t v;
    int ret;

    ret = sg_mbx_read_block(s, SG_CARDXML_URL_ADDR(0), b, 4, timeout_ms);
    printf("CardXML probe block4 @0x190: %d, data %02x %02x %02x %02x\n", ret, b[0], b[1], b[2], b[3]);
    print_last_reply(s, "block4");
    ret = sg_mbx_read(s, SG_CARDXML_URL_ADDR(0), &v, timeout_ms, 0);
    printf("CardXML probe strict @0x190: %d, value 0x%08x\n", ret, ret ? 0u : v);
    print_last_reply(s, "strict");
    ret = sg_mbx_read(s, SG_L_DDR_STATUS, &v, timeout_ms, 0);
    printf("CardXML probe strict DDR after: %d, value %u\n", ret, ret ? 0u : v);
}

static int dump_card_xml(struct sg_capture_session *s, const char *dir, uint32_t timeout_ms)
{
    char url[SG_CARDXML_URL_LEN + 1], name[128], path[512], tmp[540];
    uint32_t addr = 0, len = 0, i;
    unsigned char *data;
    int kind, swapped = 0, ret;
    time_t t0;
    FILE *f;

    puts("== Card XML ==");
    ret = sg_cardxml_read(s, SG_CARDXML_URL_ADDR(0), url, SG_CARDXML_URL_LEN, timeout_ms);
    print_last_reply(s, "first URL read (256-byte block)");
    if (ret) {
        printf("CardXML FAILED: read URL at 0x%x: %s (%d)\n", SG_CARDXML_URL_ADDR(0), strerror(-ret), ret);
        diagnose_url_read(s, timeout_ms);
        return ret;
    }
    url[SG_CARDXML_URL_LEN] = 0;
    kind = sg_cardxml_parse_url(url, name, sizeof name, &addr, &len);
    if (kind < 0) {                       /* 字节序可能反了：每个字倒过来再试一次 */
        swap_words((unsigned char *)url, SG_CARDXML_URL_LEN);
        kind = sg_cardxml_parse_url(url, name, sizeof name, &addr, &len);
        swapped = kind >= 0;
        if (kind < 0) swap_words((unsigned char *)url, SG_CARDXML_URL_LEN);
    }
    printf("CardXML url raw:");
    for (i = 0; i < 48; ++i) printf(" %02x", (unsigned char)url[i]);
    printf("\n");
    for (i = 0; i < SG_CARDXML_URL_LEN && url[i]; ++i)
        if ((unsigned char)url[i] < 0x20 || (unsigned char)url[i] > 0x7e) url[i] = '?';
    printf("CardXML url: %s%s\n", url, swapped ? "   (bytes swapped per word)" : "");
    if (kind == SG_CARDXML_FILE) {
        printf("CardXML FAILED: URL says the XML is a local file on the Windows PC (%s), not on the card\n", name);
        return -ENOENT;
    }
    if (kind < 0) {
        puts("CardXML FAILED: URL is not \"Local:<name>;<addr>;<len>\"");
        return -EPROTO;
    }
    printf("CardXML file %s at 0x%08x, %u bytes; reading 256 bytes per mailbox request...\n", name, addr, len);
    fflush(stdout);

    data = malloc(len);
    if (!data) return -ENOMEM;
    t0 = time(NULL);
    ret = sg_cardxml_read(s, addr, data, len, timeout_ms);
    if (!ret && swapped) swap_words(data, len);
    if (ret) {
        printf("CardXML FAILED: read data: %s (%d)\n", strerror(-ret), ret);
        print_last_reply(s, "data");
        free(data);
        return ret;
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) || rename(tmp, path)) {
        ret = -errno;
        printf("CardXML FAILED: write %s: %s (%d)\n", path, strerror(errno), ret);
        if (f) remove(tmp);
        free(data);
        return ret ? ret : -EIO;
    }
    free(data);
    printf("CardXML saved: %s (%u bytes, %ld s)\n", path, len, (long)(time(NULL) - t0));
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    const char *card_xml_dir = NULL;
    struct sg_linux_dev dev;
    struct sg_capture_io io;
    struct sg_capture_session session;
    struct sg_capture_options options;
    struct sg_capture_preflight preflight;
    int i, ret, exit_code;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device = argv[++i];
        } else if (!strcmp(argv[i], "--card-xml") && i + 1 < argc) {
            card_xml_dir = argv[++i];
        } else {
            usage();
            return EXIT_OPEN_FAIL;
        }
    }

    sg_capture_default_options(&options);
    memset(&preflight, 0, sizeof(preflight));

    /* 不传取消标志（NULL）：程序很短，Ctrl+C 用默认行为直接结束即可。 */
    ret = sg_linux_open(&dev, device, NULL);
    if (ret) {
        fprintf(stderr, "sg_status: cannot open %s: %s (%d)\n", device, strerror(-ret), ret);
        return EXIT_OPEN_FAIL;
    }

    sg_linux_io(&dev, &io);
    ret = sg_capture_session_init(&session, &io);
    if (ret) {
        fprintf(stderr, "sg_status: session init failed: %s (%d)\n", strerror(-ret), ret);
        sg_linux_close(&dev);
        return EXIT_OPEN_FAIL;
    }

    print_driver_info(&dev.info);

    ret = sg_capture_preflight(&session, options.control_timeout_ms, &preflight);
    print_preflight(&preflight, ret);
    exit_code = ret ? EXIT_PREFLIGHT : EXIT_OK;

    /* 预检失败也照样读这两个寄存器，它们能帮助判断原因。 */
    print_registers(&session, options.control_timeout_ms);

    if (card_xml_dir && dump_card_xml(&session, card_xml_dir, options.control_timeout_ms) && !exit_code)
        exit_code = EXIT_CARD_XML;

    sg_linux_close(&dev);
    return exit_code;
}
