/* SPDX-License-Identifier: MIT
 *
 * cam_set.h —— `sg3000-capture camera-set` 允许写哪些相机寄存器、写什么值（纯函数，可单测）。
 *
 * 地址、值域、锁定条件全部来自这台 16K 相机自己的 XML
 * （docs/vendor/camera_xml/SF6000V300R001C22XB003.xml，2026-11-14 从相机读出）：
 *
 *   名字               地址        板上输入            写入寄存器的值              条件（照 XML）
 *   ClConfiguration    0x01501008  0 Base / 3 EightyBit 同左
 *   DeviceTapGeometry  0x01501010  2 / 3                同左                       相机为 Base + Mono8（ClConfigurationRegLocked）
 *   TestPattern        0x01100000  0 1 2 3 5            同左                       XML 只有 Off/Black/White/GreyHorizontalRamp/…Moving
 *   ExposureTime       0x01001010  微秒（可带小数）     µs × 96（SensorClockFrequency）  在相机给的 [0x01001014, 0x01001018] 内（随行频变）
 *   DigitalGain        0x0120604c  倍数 0.10..15.99     倍数 × 256                 —
 *   AnalogGain         0x01001c00  0..4 = 1.25/1.75/2/3/4 倍  同左                   BinningV（0x01302040）不为 1、2（SensorBinVerLock）
 *   LineRate           0x0100101c  行频 Hz（可带小数）  96e6 / Hz（时钟数）       在相机给的 [0x01001020, 0x01001024] 内；LineTriggerMode（0x01001004）= 0
 *
 * 只改运行时值：从不写 UserSetSave（0x01600318），相机断电恢复 UserSet1。
 */
#ifndef CAM_SET_H
#define CAM_SET_H
#include <stdint.h>

#define CAM16K_MODEL              "SW-WS2-CL16K-50M-SDE"
#define CAM16K_CL_CONFIG          0x01501008u
#define CAM16K_TAP_GEOMETRY       0x01501010u
#define CAM16K_PIXEL_FORMAT       0x01401040u
#define CAM16K_TEST_PATTERN       0x01100000u
#define CAM16K_EXPOSURE_CLK       0x01001010u
#define CAM16K_EXPOSURE_MIN_CLK   0x01001014u   /* RO */
#define CAM16K_EXPOSURE_MAX_CLK   0x01001018u   /* RO */
#define CAM16K_DIGITAL_GAIN       0x0120604cu
#define CAM16K_ANALOG_GAIN        0x01001c00u
#define CAM16K_BINNING_V          0x01302040u
#define CAM16K_LINE_PERIOD_CLK    0x0100101cu
#define CAM16K_LINE_PERIOD_MIN    0x01001020u   /* RO，时钟数，随宽度 / CL 模式 / 时钟变 */
#define CAM16K_LINE_PERIOD_MAX    0x01001024u   /* RO */
#define CAM16K_LINE_TRIGGER_MODE  0x01001004u
#define CAM16K_MONO8              0x01080001u
#define CAM16K_SENSOR_MHZ         96u

/* 写之前从相机读到的状态（锁定条件和曝光范围要用）。 */
struct cam_set_state {
    uint32_t cl_config, pixel_format, binning_v, exposure_min_clk, exposure_max_clk;
    uint32_t line_period_min_clk, line_period_max_clk, line_trigger_mode;
};

/* 名字 → 地址。不认识的名字返回 0。 */
uint32_t cam_set_address(const char *name);

/* 检查并换算：text 是命令行上的值。成功返回 0，*raw = 要写进寄存器的值；
 * 名字或格式不对 -EINVAL；值或当前状态不允许 -EPERM（why 指向原因说明）。 */
int cam_set_check(const char *name, const char *text, const struct cam_set_state *st,
                  uint32_t *raw, const char **why);

#endif
