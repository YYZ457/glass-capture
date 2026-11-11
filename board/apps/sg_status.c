/* SPDX-License-Identifier: MIT
 *
 * sg_status —— 只读体检程序（零风险：不启动采集、不写任何控制寄存器）。
 *
 * 用法：sg_status [--device PATH]
 * 输出：
 *   1. 驱动信息（SG_IOC_INFO）：PCIe 位置、ABI、引擎 ID、对齐要求、poisoned
 *   2. 预检结果：DDR 状态、时钟寄存器（逐位打印）、CL 配置、彩色、位深
 *   3. SG_HDR_READY、SG_ACQ_STATE 的当前值
 * 退出码：0 一切正常；1 预检不通过；2 打不开设备
 *
 * 组装：sg_io_linux（打开设备）→ sg_session → sg_preflight
 * 没有 legacy 来源；可参考 legacy sg3000_capture.c 的 "preflight" 子命令。
 *
 * 这个程序里只有"读"：
 *   - 邮箱读（预检五项）
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
#include "sg3000_regs.h"
#include "sg_err.h"

/* 退出码 */
#define EXIT_OK          0
#define EXIT_PREFLIGHT   1
#define EXIT_OPEN_FAIL   2

static void usage(void)
{
    puts("Usage: sg_status [--device PATH]\n"
         "Read-only check: driver info, preflight, header-ready and acquisition state.\n"
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

static void print_registers(struct sg_capture_session *s)
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
}

int main(int argc, char **argv)
{
    const char *device = SG_DEFAULT_DEVICE;
    struct sg_linux_dev dev;
    struct sg_capture_io io;
    struct sg_capture_session session;
    struct sg_capture_options options;
    struct sg_capture_preflight preflight;
    int i, ret, exit_code;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device = argv[++i];
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
    print_registers(&session);

    sg_linux_close(&dev);
    return exit_code;
}
