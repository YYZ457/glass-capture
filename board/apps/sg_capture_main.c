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

#include "sg_io_linux.h"
#include "sg_acq.h"
#include "pgm_write.h"
#include "tx_tcp.h"
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
         "       sg3000-capture capture --output PATH [--device PATH] [--timeout-ms 1..60000] [--send IP[:PORT]]\n"
         "       sg3000-capture send FILE.pgm --to IP[:PORT]\n"
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
        if (!ret) {
            ret = pgm_write(output, &result.frame);
            if (!ret) {
                printf("Saved complete Mono8 frame: %u x %u, %zu bytes, block %u, %u packets: %s\n",
                       result.frame.width, result.frame.height, result.frame.length,
                       result.frame.block_id, result.frame.packets, output);
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
