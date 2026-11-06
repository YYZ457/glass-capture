/* SPDX-License-Identifier: MIT
 *
 * sg_session.c —— 会话初始化、默认参数、帧释放、读写小工具。
 * 职责：只管"会话"这个容器本身，不碰任何寄存器含义。
 * 依赖：sg_capture_io.h（回调表）
 * 被谁用：capture/ 下所有文件、apps/
 *
 * 迁移来源：legacy/sg3000-core-v0.2/sg3000_capture_core.c
 *   sg_session_cancelled / rd / wr      ← 第 18-23 行
 *   sg_capture_default_options          ← 第 25-29 行
 *   sg_capture_session_init             ← 第 30-35 行
 *   sg_capture_frame_free               ← 第 36-37 行
 * 怎么验证：电脑上用假回调表调用，缺任一必需回调应返回 -EINVAL。
 */
#include "sg_session.h"
#include "sg_err.h"

int sg_session_cancelled(struct sg_capture_session *s) { (void)s; return 0; /* TODO */ }
int sg_session_rd(struct sg_capture_session *s, uint32_t off, uint32_t *v)
{ (void)s; (void)off; (void)v; return -ENOSYS; /* TODO */ }
int sg_session_wr(struct sg_capture_session *s, uint32_t off, uint32_t v)
{ (void)s; (void)off; (void)v; return -ENOSYS; /* TODO */ }

void sg_capture_default_options(struct sg_capture_options *o) { (void)o; /* TODO */ }

int sg_capture_session_init(struct sg_capture_session *s, const struct sg_capture_io *io)
{ (void)s; (void)io; return -ENOSYS; /* TODO */ }

void sg_capture_frame_free(struct sg_capture_frame *frame) { (void)frame; /* TODO */ }
