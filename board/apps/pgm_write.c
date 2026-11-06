/* SPDX-License-Identifier: MIT
 * pgm_write.c —— 迁移来源见 pgm_write.h。
 * 怎么验证：写出的文件头为 "P5\n<w> <h>\n255\n"，文件大小 = 头长度 + w×h。
 */
#include "pgm_write.h"
#include "sg_err.h"

int pgm_write(const char *path, const struct sg_capture_frame *frame)
{ (void)path; (void)frame; return -ENOSYS; /* TODO */ }
