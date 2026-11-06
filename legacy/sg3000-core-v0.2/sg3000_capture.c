/* SPDX-License-Identifier: MIT
 * Experimental Linux client for the dedicated sg3000_rx ioctl backend.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sg3000_capture_core.h"
#include "sg3000_rx_uapi.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0) /* Linux renameat2 ABI, older libc headers. */
#endif

static volatile sig_atomic_t interrupted;
struct backend { int fd; };
static void interrupt_handler(int number) { (void)number; interrupted = 1; }
static int reg_read(void *context, uint32_t offset, uint32_t *value)
{
    struct backend *b = context; struct sg_reg reg = {offset, 0};
    if (ioctl(b->fd, SG_IOC_READ_REG, &reg) < 0) return -errno;
    *value = reg.value; return 0;
}
static int reg_write(void *context, uint32_t offset, uint32_t value)
{
    struct backend *b = context; struct sg_reg reg = {offset, value};
    return ioctl(b->fd, SG_IOC_WRITE_REG, &reg) < 0 ? -errno : 0;
}
static int dma_read(void *context, uint32_t source, void *buffer,
                    uint32_t length, uint32_t timeout)
{
    struct backend *b = context;
    struct sg_dma dma = {0};
    dma.source = source; dma.buffer = (uint64_t)(uintptr_t)buffer;
    dma.length = length; dma.timeout_ms = timeout;
    if (ioctl(b->fd, SG_IOC_DMA_READ, &dma) < 0) return -errno;
    if (dma.completed != 1 || (dma.status & (0x00f80638u | 1u))) return -EIO;
    return 0;
}
static uint64_t monotonic_ms(void *context)
{
    struct timespec t; (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void pause_ms(void *context, uint32_t milliseconds)
{
    struct timespec pause = {milliseconds / 1000u, (long)(milliseconds % 1000u) * 1000000L};
    (void)context; (void)nanosleep(&pause, NULL);
}
static int is_cancelled(void *context) { (void)context; return interrupted != 0; }
static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t amount;
        if (interrupted) return -ECANCELED;
        amount = write(fd, p, length);
        if (amount < 0) { if (errno == EINTR) continue; return -errno; }
        if (!amount) return -EIO;
        p += amount; length -= (size_t)amount;
    }
    return 0;
}
/* Only publish a completed PGM: write a private sibling temporary file, sync,
 * then rename it atomically without replacement (also works on FAT32).
 * Older kernels can fall back to a non-replacing hard link.
 * A failed/cancelled write cannot leave a partial file at the requested path.
 */
static int save_pgm(const char *path, const struct sg_capture_frame *frame)
{
    char header[96], *temporary; int fd, ret, n, renamed = 0;
    size_t path_length = strlen(path);
    if (!frame->data || !frame->length || !frame->width || !frame->height ||
        (uint64_t)frame->width * frame->height != frame->length) return -EINVAL;
    if (path_length > SIZE_MAX - 32) return -ENAMETOOLONG;
    temporary = malloc(path_length + 32); if (!temporary) return -ENOMEM;
    snprintf(temporary, path_length + 32, "%s.sg3000-XXXXXX", path);
    fd = mkstemp(temporary); if (fd < 0) { ret = -errno; free(temporary); return ret; }
    n = snprintf(header, sizeof(header), "P5\n%u %u\n255\n", frame->width, frame->height);
    ret = n < 0 || (size_t)n >= sizeof(header) ? -EOVERFLOW : write_all(fd, header, (size_t)n);
    if (!ret) ret = write_all(fd, frame->data, frame->length);
    if (!ret && fsync(fd)) ret = -errno;
    if (close(fd) && !ret) ret = -errno;
    if (!ret && interrupted) ret = -ECANCELED;
    if (!ret) {
#ifdef SYS_renameat2
        if (!syscall(SYS_renameat2, AT_FDCWD, temporary, AT_FDCWD, path,
                     RENAME_NOREPLACE)) renamed = 1;
        else if (errno == ENOSYS || errno == EINVAL) {
            if (link(temporary, path)) ret = -errno;
        } else ret = -errno;
#else
        if (link(temporary, path)) ret = -errno;
#endif
    }
    /* The target, if published, is already complete despite cleanup failure. */
    if (!renamed && unlink(temporary))
        fprintf(stderr, "Temporary file cleanup failed: %s\n", temporary);
    free(temporary); return ret;
}
static void usage(void)
{
    puts("Usage: sg3000-capture preflight [--device PATH]\n"
         "       sg3000-capture capture --output PATH [--device PATH] [--timeout-ms 1..60000]\n"
         "Default device: /dev/sg3000_rx_0000:01:00.0\n"
         "Capture explicitly enables the experimental port-0 acquisition sequence.");
}
int main(int argc, char **argv)
{
    const char *device = "/dev/sg3000_rx_0000:01:00.0", *output = NULL;
    struct backend backend = {-1}; struct sg_info info = {0};
    struct sg_capture_options options; struct sg_capture_session session;
    struct sg_capture_result result; struct sg_capture_preflight preflight;
    struct sg_capture_io io; struct sigaction action;
    int capture, ret = 0, lockfd = -1, i; struct stat st;
    sg_capture_default_options(&options);
    if (argc < 2 || (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help"))) { usage(); return argc < 2; }
    capture = !strcmp(argv[1], "capture");
    if (!capture && strcmp(argv[1], "preflight")) { usage(); return 2; }
    for (i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc && capture && !output) output = argv[++i];
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc && capture) {
            char *end; unsigned long value;
            errno = 0; value = strtoul(argv[++i], &end, 10);
            if (errno || *end || !value || value > 60000) { usage(); return 2; }
            options.frame_timeout_ms = (uint32_t)value;
        } else { usage(); return 2; }
    }
    if (capture && (!output || !*output)) { usage(); return 2; }
    if (output) {
        if (!lstat(output, &st)) { fprintf(stderr, "Output already exists; refusing overwrite\n"); return 1; }
        if (errno != ENOENT) { perror("output path"); return 1; }
    }
    memset(&action, 0, sizeof(action)); action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) { perror("sigaction"); return 1; }
    lockfd = open("/run/sg3000-control.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB)) { perror("exclusive control lock"); ret = -EBUSY; goto done; }
    backend.fd = open(device, O_RDWR | O_CLOEXEC);
    if (backend.fd < 0) { ret = -errno; goto done; }
    if (ioctl(backend.fd, SG_IOC_INFO, &info) < 0) { ret = -errno; goto done; }
    if (info.abi != SG_ABI_VERSION || info.vendor != 0x4c54 || info.device != 0x3000 ||
        info.poisoned || info.max_transfer < SG_CAPTURE_DMA_CHUNK ||
        (info.engine_id >> 16) != 0x1fc1u || (info.engine_id & 0x8f00u)) {
        ret = -ENODEV; goto done;
    }
    printf("SG3000 %04x:%02x:%02x.%u, C2H0 AXI MM, ABI %u\n", info.domain,
           info.bus, info.devfn >> 3, info.devfn & 7u, info.abi);
    io = (struct sg_capture_io){&backend, reg_read, reg_write, dma_read,
                              monotonic_ms, pause_ms, is_cancelled};
    ret = sg_capture_session_init(&session, &io); if (ret) goto done;
    if (!capture) {
        ret = sg_capture_preflight(&session, options.control_timeout_ms, &preflight);
        printf("DDRInitialStatus=%u, CameraLinkPixelClocks=0x%08x, CL=%u, Color=%u, Depth=%u\n",
               preflight.ddr_status, preflight.pixel_clocks, preflight.cl_configuration,
               preflight.camera_color, preflight.camera_depth);
        if (!ret) puts("Preflight passed. Acquisition and DMA were not started.");
    } else {
        ret = sg_capture_once(&session, &options, &result);
        printf("DDRInitialStatus=%u, CameraLinkPixelClocks=0x%08x, CL=%u, Color=%u, Depth=%u\n",
               result.preflight.ddr_status, result.preflight.pixel_clocks,
               result.preflight.cl_configuration, result.preflight.camera_color,
               result.preflight.camera_depth);
        if (result.stop_error) fprintf(stderr, "AcquisitionStop cleanup failed: %s (%d)\n",
                                      strerror(-result.stop_error), result.stop_error);
        if (!ret) {
            ret = save_pgm(output, &result.frame);
            if (!ret) printf("Saved complete Mono8 frame: %u x %u, %zu bytes, block %u, %u packets: %s\n",
                             result.frame.width, result.frame.height, result.frame.length,
                             result.frame.block_id, result.frame.packets, output);
        }
        sg_capture_frame_free(&result.frame);
    }
done:
    if (backend.fd >= 0) close(backend.fd);
    if (lockfd >= 0) close(lockfd);
    if (ret) fprintf(stderr, "Failed: %s (%d)\n", strerror(-ret), ret);
    return ret ? 1 : 0;
}
