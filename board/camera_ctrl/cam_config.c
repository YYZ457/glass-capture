/* SPDX-License-Identifier: MIT
 *
 * cam_config.c —— ⬜❓ 依赖 cam_serial 和相机命令表。
 * 怎么验证：cam_set 之后 cam_get 读回一致；打开测试图案后跑 verify/V1_testpattern.py。
 */
#include "cam_config.h"
#include "cam_serial.h"
#include "sg_err.h"

int cam_get(struct sg_capture_session *s, struct cam_settings *out)
{ (void)s; (void)out; return -ENOSYS; }
int cam_set(struct sg_capture_session *s, const struct cam_settings *in)
{ (void)s; (void)in; return -ENOSYS; }
int cam_save_user_set(struct sg_capture_session *s, unsigned slot)
{ (void)s; (void)slot; return -ENOSYS; }
