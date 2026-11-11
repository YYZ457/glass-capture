/* SPDX-License-Identifier: MIT
 *
 * pgm_write.h —— 把一块 Mono8 数据存成 P5 PGM 文件（原子写：先写临时文件再改名，不覆盖已有文件）。
 * 迁移来源：legacy sg3000_capture.c 第 23、63-112 行（RENAME_NOREPLACE、write_all、save_pgm）
 * 状态：✅
 */
#ifndef PGM_WRITE_H
#define PGM_WRITE_H
#include "sg_frame.h"
int pgm_write(const char *path, const struct sg_capture_frame *frame);

/* 读回一个 P5 / maxval 255 的 PGM（sg3000-capture send 用）。
 * 成功后 frame->data 由调用者 sg_capture_frame_free；max_bytes 是像素字节上限。 */
int pgm_read(const char *path, struct sg_capture_frame *frame, size_t max_bytes);
#endif
