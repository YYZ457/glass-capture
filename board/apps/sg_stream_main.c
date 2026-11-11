/* SPDX-License-Identifier: MIT
 *
 * sg3000-stream —— 连续采集 + 处理程序。
 *
 * 组装：
 *   数据源   sg_source      ← 硬件（sg_io_linux + sg_acq 的 begin/next/end）或 合成（--source synthetic）
 *   处理器   sg_processor   ← sg_stats
 *   出口     sg_result_sink ← tx_jsonl
 *   引擎     sg_pipeline_run
 *
 * 迁移来源：legacy sg_stream_main.c。命令行和输出的 SGJSON 行与 legacy 一致，
 *   区别：去掉 --processor pl（D001）；新增 --device。
 * 怎么验证：先 --source synthetic 跑通，再接硬件跑 100 块。
 * 状态：🟡 合成源电脑上跑过；硬件源从没在实机上跑过
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sg_io_linux.h"
#include "sg_acq.h"
#include "sg_pipeline.h"
#include "tx_jsonl.h"
#include "sg_err.h"

static volatile sig_atomic_t stopped;

static void on_signal(int number)
{
    (void)number;
    stopped = 1;
}

/* ==== 两种数据源 ======================================================= */

/* 硬件源：直接包一层 sg_stream_next / sg_stream_end */
struct hardware {
    struct sg_linux_dev dev;
    struct sg_capture_session session;
    struct sg_capture_stream stream;
};

static int hardware_next(void *context, struct sg_capture_frame *frame)
{
    struct hardware *h = context;
    return sg_stream_next(&h->stream, frame);
}

static int hardware_stop(void *context)
{
    struct hardware *h = context;
    return sg_stream_end(&h->stream);
}

/* 合成源：每隔 delay 毫秒造一块 (i + 块号) 的渐变图，只用于测试 */
struct synthetic {
    uint32_t width, height, block, delay;
};

static int synthetic_next(void *context, struct sg_capture_frame *frame)
{
    struct synthetic *s = context;
    size_t n = (size_t)s->width * s->height, i;
    struct timespec pause = { s->delay / 1000u, (long)(s->delay % 1000u) * 1000000L };

    nanosleep(&pause, NULL);
    if (stopped) {
        return -ECANCELED;
    }
    memset(frame, 0, sizeof(*frame));
    frame->data = malloc(n);
    if (frame->data == NULL) {
        return -ENOMEM;
    }
    s->block++;
    for (i = 0; i < n; ++i) {
        frame->data[i] = (unsigned char)(i + s->block);
    }
    frame->width    = s->width;
    frame->height   = s->height;
    frame->length   = n;
    frame->block_id = s->block;
    frame->packets  = 1;
    return 0;
}

static int synthetic_stop(void *context)
{
    (void)context;
    return 0;
}

/* ==== 参数 ============================================================= */

/* 解析非负整数；不合法直接退出（退出码 2）。 */
static uint64_t number(const char *s, uint64_t max)
{
    unsigned long long value;
    char *end;

    if (!s || !*s || *s == '-' || *s == '+') {
        fprintf(stderr, "Invalid number\n");
        exit(2);
    }
    errno = 0;
    value = strtoull(s, &end, 10);
    if (errno || *end || value > max) {
        fprintf(stderr, "Number out of range\n");
        exit(2);
    }
    return value;
}

static void usage(void)
{
    puts("sg3000-stream 0.2 / interface ABI 1\n"
         "--source hardware|synthetic --processor stats --seconds 30 (0=until stopped)\n"
         "--max-blocks 0 --queue 4 --max-frame-mib 16 --block-policy observe|strict\n"
         "--process-timeout-ms 2000 --emit-ms 1000 --threshold 128 --device PATH\n"
         "Synthetic only: --width 256 --height 64 --synthetic-delay-ms 50\n"
         "Output: SGJSON lines to stdout; no automatic image/result files or external actuation.");
}

/* ==== 硬件源的准备：打开 → 会话 → begin → 打印 preflight 事件 =========== */

static int hardware_start(struct hardware *h, const char *device, uint64_t max_frame_bytes,
                          int *begin_attempted)
{
    struct sg_capture_options capture;
    struct sg_capture_io io;
    char line[400];
    int ret, e;

    ret = sg_linux_open(&h->dev, device, &stopped);
    if (ret) {
        return ret;
    }
    sg_linux_io(&h->dev, &io);
    sg_capture_default_options(&capture);
    capture.max_frame_bytes = (size_t)max_frame_bytes;
    ret = sg_capture_session_init(&h->session, &io);
    if (ret) {
        return ret;
    }

    *begin_attempted = 1;
    ret = sg_stream_begin(&h->stream, &h->session, &capture);
    snprintf(line, sizeof(line),
             "SGJSON {\"schema\":1,\"event\":\"preflight\",\"valid_mask\":%u,\"ddr\":%u,"
             "\"clocks\":%u,\"error\":%d}\n",
             h->stream.preflight.valid_mask, h->stream.preflight.ddr_status,
             h->stream.preflight.pixel_clocks, ret);
    e = tx_jsonl_emit(line, 2000);
    return ret ? ret : e;
}

int main(int argc, char **argv)
{
    static const struct option args[] = {
        {"source", 1, 0, 's'}, {"processor", 1, 0, 'p'}, {"seconds", 1, 0, 'd'},
        {"max-blocks", 1, 0, 'n'}, {"queue", 1, 0, 'q'}, {"max-frame-mib", 1, 0, 'm'},
        {"block-policy", 1, 0, 'b'}, {"process-timeout-ms", 1, 0, 't'},
        {"emit-ms", 1, 0, 'e'}, {"threshold", 1, 0, 'r'}, {"width", 1, 0, 'w'},
        {"height", 1, 0, 'h'}, {"synthetic-delay-ms", 1, 0, 'i'}, {"device", 1, 0, 'D'},
        {"version", 0, 0, 'v'}, {"help", 0, 0, '?'}, {0, 0, 0, 0}};
    const char *source_name = "hardware", *processor_name = "stats";
    const char *device = SG_DEFAULT_DEVICE;
    struct sg_pipeline_options o = {
        .queue_depth = 4, .processing_timeout_ms = 2000, .max_blocks = 0,
        .duration_ms = 30000, .max_frame_bytes = 16u * 1024u * 1024u, .cancel = &stopped };
    struct synthetic synthetic = { 256, 64, 0, 50 };
    struct hardware hardware;
    struct sg_processor processor = { 0 };
    struct sg_source source = { 0 };
    struct tx_jsonl output = { 0 };
    struct sg_result_sink sink;
    struct sg_pipeline_stats stats = { 0 };
    struct sigaction action;
    uint32_t threshold = 128;
    uint64_t emit_ms = 1000;
    int c, ret = 0, old_flags = -1, begin_attempted = 0, e;
    char line[1800];

    memset(&hardware, 0, sizeof(hardware));
    hardware.dev.fd = hardware.dev.lock_fd = -1;

    /* ---- 解析命令行 ---- */
    while ((c = getopt_long(argc, argv, "", args, NULL)) != -1) {
        switch (c) {
        case 's': source_name = optarg; break;
        case 'p': processor_name = optarg; break;
        case 'd': o.duration_ms = number(optarg, 86400) * 1000; break;
        case 'n': o.max_blocks = number(optarg, UINT64_MAX); break;
        case 'q': o.queue_depth = (unsigned)number(optarg, 16); break;
        case 'm': o.max_frame_bytes = number(optarg, 256) * 1024u * 1024u; break;
        case 'b':
            if (strcmp(optarg, "observe") && strcmp(optarg, "strict")) {
                return 2;
            }
            o.strict_block_ids = !strcmp(optarg, "strict");
            break;
        case 't': o.processing_timeout_ms = (unsigned)number(optarg, 60000); break;
        case 'e': emit_ms = number(optarg, 60000); break;
        case 'r': threshold = (uint32_t)number(optarg, 255); break;
        case 'w': synthetic.width = (uint32_t)number(optarg, 32768); break;
        case 'h': synthetic.height = (uint32_t)number(optarg, 32768); break;
        case 'i': synthetic.delay = (uint32_t)number(optarg, 10000); break;
        case 'D': device = optarg; break;
        case 'v': puts("sg3000-stream 0.2 ABI 1"); return 0;
        default:
            usage();
            return c == '?' && optind > 1 && !strcmp(argv[optind - 1], "--help") ? 0 : 2;
        }
    }
    if (optind != argc ||
        (strcmp(source_name, "hardware") && strcmp(source_name, "synthetic")) ||
        strcmp(processor_name, "stats") ||
        !o.queue_depth || !o.max_frame_bytes || !o.processing_timeout_ms || !emit_ms ||
        o.max_frame_bytes * (o.queue_depth + 2u) > UINT64_C(512) * 1024 * 1024 ||
        !synthetic.width || !synthetic.height ||
        (uint64_t)synthetic.width * synthetic.height > o.max_frame_bytes) {
        usage();
        return 2;
    }
    o.synthetic = !strcmp(source_name, "synthetic");

    /* ---- 信号；stdout 设成非阻塞，写满时由 tx_jsonl 按超时处理 ---- */
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    old_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (old_flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, old_flags | O_NONBLOCK)) {
        return 1;
    }

    output.source = source_name;
    output.processor = processor_name;
    output.interval_ns = emit_ms * 1000000u;
    tx_jsonl_sink(&output, &sink);

    /* ---- 组装 ---- */
    ret = sg_stats_open(&processor, threshold);
    if (ret) {
        goto done;
    }
    if (o.synthetic) {
        source = (struct sg_source){ &synthetic, synthetic_next, synthetic_stop };
    } else {
        ret = hardware_start(&hardware, device, o.max_frame_bytes, &begin_attempted);
        if (ret) {
            goto done;
        }
        source = (struct sg_source){ &hardware, hardware_next, hardware_stop };
    }

    snprintf(line, sizeof(line),
             "SGJSON {\"schema\":1,\"event\":\"started\",\"source\":\"%s\",\"processor\":\"%s\","
             "\"queue_depth\":%u,\"max_frame_bytes\":\"%" PRIu64 "\",\"continuity_verified\":false}\n",
             source_name, processor_name, o.queue_depth, o.max_frame_bytes);
    ret = tx_jsonl_emit(line, 2000);
    if (ret) {
        goto done;
    }

    /* ---- 干活 ---- */
    ret = sg_pipeline_run(&source, &processor, &sink, &o, &stats);

done:
    /* end 是幂等的：流水线里已经调过 stop 的话，这里什么都不做 */
    if (begin_attempted) {
        e = sg_stream_end(&hardware.stream);
        if (e) {
            stats.stop_error = e;
            if (!ret) {
                ret = e;
            }
        }
    }
    snprintf(line, sizeof(line),
             "SGJSON {\"schema\":1,\"event\":\"ended\",\"source\":\"%s\",\"processor\":\"%s\","
             "\"error\":%d,\"source_error\":%d,\"processing_error\":%d,\"output_error\":%d,"
             "\"stop_error\":%d,\"cancelled\":%s,\"received\":\"%" PRIu64 "\","
             "\"processed\":\"%" PRIu64 "\",\"bytes\":\"%" PRIu64 "\","
             "\"id_discontinuities\":\"%" PRIu64 "\",\"queue_full_events\":\"%" PRIu64 "\","
             "\"queue_peak\":%u,\"elapsed_ms\":\"%" PRIu64 "\"}\n",
             source_name, processor_name, ret, stats.source_error, stats.processing_error,
             stats.output_error, stats.stop_error, stats.cancelled ? "true" : "false",
             stats.received, stats.processed, stats.bytes, stats.id_discontinuities,
             stats.queue_full_events, stats.queue_peak,
             (stats.finished_ns - stats.started_ns) / 1000000u);
    e = tx_jsonl_emit(line, 2000);
    if (!ret) {
        ret = e;
    }

    if (processor.close) {
        processor.close(processor.context);
    }
    sg_linux_close(&hardware.dev);
    if (old_flags >= 0) {
        fcntl(STDOUT_FILENO, F_SETFL, old_flags);
    }
    if (ret) {
        fprintf(stderr, "Stream failed: %s (%d)\n", strerror(-ret), ret);
    }
    return ret ? 1 : 0;
}
