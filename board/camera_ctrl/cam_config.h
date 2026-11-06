/* SPDX-License-Identifier: MIT
 *
 * cam_config.h —— 相机的高层配置：用"意思"而不是"命令字符串"来设置相机。⬜❓
 * 依赖：cam_serial（发命令）。命令表来自 docs/interfaces/I7_camera_serial.md。
 *
 * 目标值（来自项目要求）：宽 16384、行频约 15.4 kHz（75 mm/s ÷ 4.88 µm）、Mono8
 */
#ifndef CAM_CONFIG_H
#define CAM_CONFIG_H
#include <stdint.h>
#include "sg_session.h"

enum cam_trigger { CAM_TRIG_FREE_RUN = 0, CAM_TRIG_EXTERNAL = 1 /* 编码器 */ };

struct cam_settings {
    uint32_t width;            /* 像素 */
    uint32_t line_rate_hz;
    uint32_t exposure_us;
    enum cam_trigger trigger;
    int test_pattern;          /* 0 = 关；其他值对应相机手册里的图案编号 */
};

int cam_get(struct sg_capture_session *s, struct cam_settings *out);
int cam_set(struct sg_capture_session *s, const struct cam_settings *in);
int cam_save_user_set(struct sg_capture_session *s, unsigned slot);

#endif
