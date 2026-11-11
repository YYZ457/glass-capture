/* SPDX-License-Identifier: MIT
 * pgm_write.c —— 迁移来源见 pgm_write.h。
 * 怎么验证：写出的文件头为 "P5\n<w> <h>\n255\n"，文件大小 = 头长度 + w×h。
 *
 * 为什么要"先写临时文件再改名"：
 *   如果直接往目标文件里写，写到一半断电或被打断，目标路径上就留下一张残缺的图，
 *   看起来像是正常文件。所以先写到旁边的临时文件，全部写完并 fsync 之后，
 *   再用 rename 一步改成目标名字。目标路径上要么没有文件，要么是完整的文件。
 *   rename 用 RENAME_NOREPLACE：目标已存在就失败，不会覆盖已有的图。
 *
 * 与 legacy 的一处区别：legacy 的 save_pgm 在写文件途中也检查 Ctrl+C 标志。
 * pgm_write(path, frame) 的接口里没有取消标志，所以这里写文件途中不响应取消；
 * 结果仍然是完整的文件（Ctrl+C 只会让正在进行的 write 提前返回，重试即可）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "pgm_write.h"
#include "sg_err.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)  /* Linux renameat2 的标志；旧版 libc 头文件里没有 */
#endif

/* 把 length 字节全部写进 fd。write 可能一次只写一部分，所以要循环。 */
static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;

    while (length) {
        ssize_t amount = write(fd, p, length);

        if (amount < 0) {
            if (errno == EINTR) {
                continue;               /* 被信号打断，重试 */
            }
            return -errno;
        }
        if (amount == 0) {
            return -EIO;
        }
        p += amount;
        length -= (size_t)amount;
    }
    return 0;
}

/* 把临时文件改名为 path，且绝不覆盖已有文件。成功并且是靠改名完成的，*renamed 置 1。 */
static int publish_without_replace(const char *temporary, const char *path, int *renamed)
{
    *renamed = 0;
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, temporary, AT_FDCWD, path, RENAME_NOREPLACE) == 0) {
        *renamed = 1;
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -errno;
    }
    /* 内核太旧，不认识 renameat2：退而求其次用硬链接。link 遇到已存在的目标会失败，同样不会覆盖。
     * 链接成功后临时文件还在，由调用者删除。 */
#endif
    if (link(temporary, path)) {
        return -errno;
    }
    return 0;
}

int pgm_write(const char *path, const struct sg_capture_frame *frame)
{
    char header[96];
    char *temporary;
    size_t path_length;
    int fd, n, ret, renamed = 0;

    if (path == NULL || frame == NULL || frame->data == NULL ||
        frame->length == 0 || frame->width == 0 || frame->height == 0 ||
        (uint64_t)frame->width * frame->height != frame->length) {
        return -EINVAL;
    }

    path_length = strlen(path);
    if (path_length > SIZE_MAX - 32) {
        return -ENAMETOOLONG;
    }

    /* 临时文件和目标放在同一个目录（名字后面加后缀），这样改名才是同一文件系统内的一步操作。 */
    temporary = malloc(path_length + 32);
    if (temporary == NULL) {
        return -ENOMEM;
    }
    snprintf(temporary, path_length + 32, "%s.sg3000-XXXXXX", path);

    fd = mkstemp(temporary);
    if (fd < 0) {
        ret = -errno;
        free(temporary);
        return ret;
    }

    /* PGM 的 P5 格式：文本头 + 原始字节。 */
    n = snprintf(header, sizeof(header), "P5\n%u %u\n255\n", frame->width, frame->height);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        ret = -EOVERFLOW;
    } else {
        ret = write_all(fd, header, (size_t)n);
    }
    if (!ret) {
        ret = write_all(fd, frame->data, frame->length);
    }
    if (!ret && fsync(fd)) {            /* 确保数据真的落盘，再改名 */
        ret = -errno;
    }
    if (close(fd) && !ret) {
        ret = -errno;
    }

    if (!ret) {
        ret = publish_without_replace(temporary, path, &renamed);
    }

    /* 改名成功后临时名字已不存在；其他情况（失败，或用了硬链接）要把临时文件删掉。
     * 删除失败只提醒：此时目标路径上如果已有文件，它本身是完整的。 */
    if (!renamed && unlink(temporary)) {
        fprintf(stderr, "Temporary file cleanup failed: %s\n", temporary);
    }
    free(temporary);
    return ret;
}

/* ==== 读 PGM =========================================================== */

/* 读头里的下一个十进制数，跳过空白和 # 注释。 */
static int header_number(FILE *f, unsigned long *out)
{
    int c = fgetc(f);
    unsigned long v = 0;
    int digits = 0;

    for (;;) {
        while (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            c = fgetc(f);
        }
        if (c != '#') {
            break;
        }
        while (c != '\n' && c != EOF) {
            c = fgetc(f);
        }
    }
    while (c >= '0' && c <= '9' && digits < 9) {
        v = v * 10 + (unsigned long)(c - '0');
        digits++;
        c = fgetc(f);
    }
    /* 数字后面必须正好一个空白字符 */
    if (!digits || !(c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

int pgm_read(const char *path, struct sg_capture_frame *frame, size_t max_bytes)
{
    unsigned long width, height, maxval;
    FILE *f;
    int ret = 0;

    if (path == NULL || frame == NULL) {
        return -EINVAL;
    }
    memset(frame, 0, sizeof(*frame));
    f = fopen(path, "rb");
    if (f == NULL) {
        return -errno;
    }
    if (fgetc(f) != 'P' || fgetc(f) != '5' ||
        header_number(f, &width) || header_number(f, &height) || header_number(f, &maxval) ||
        width == 0 || height == 0 || maxval != 255 ||
        (uint64_t)width * height > max_bytes) {
        ret = -EINVAL;
        goto out;
    }
    frame->length = (size_t)width * height;
    frame->data = malloc(frame->length);
    if (frame->data == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    if (fread(frame->data, 1, frame->length, f) != frame->length) {
        ret = -EINVAL;              /* 文件比头里声明的短 */
    } else if (fgetc(f) != EOF) {
        ret = -EINVAL;              /* 文件比头里声明的长 */
    }
    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->packets = 1;

out:
    fclose(f);
    if (ret) {
        sg_capture_frame_free(frame);
    }
    return ret;
}
