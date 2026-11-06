/* SPDX-License-Identifier: MIT
 *
 * sg_view.c —— 检查一个 sg_image_view 是否合法（指针、宽高、stride、像素格式、字节数）。
 * 职责：所有处理器在动像素之前都先调它，防止越界读。
 * 迁移来源：legacy sg_processing.c 第 9-16 行（sg_view_validate）
 * 怎么验证：电脑上构造 stride < width、bytes 不够等非法输入，均应返回 -EINVAL。
 */
#include "sg_processing.h"
#include "sg_err.h"

int sg_view_validate(const struct sg_image_view *v) { (void)v; return -ENOSYS; /* TODO */ }
