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
 *   DigitalShift       0x01207040  0..4 = ×1 ×2 ×4 ×8 ×16  同左                       —（取相机内部数据的哪 8 位）
 *   TDIStage           0x01106008  0 = 1 级 / 1 = 2 级  同左                       —
 *   TDIMode            0x0110600c  0 求和 / 1 平均      同左                       —
 *   BlackLevel         0x0120c050  整数（可为负）       同左（补码）               在相机给的 [0x0120c058, 0x0120c05c] 内
 *
 *   平场校正（FFC，相机内逐像素校正；"暗场 / 亮场校正"）：
 *   FFCEnable          0x01102000  0 关 / 1 开          同左
 *   FFCUserSetSelector 0x01102d00  0 出厂 / 1..3 用户组 同左                       只是选择，不读不写闪存
 *   FFCSampleFrames    0x01102080  1024 / 2048          同左
 *   FFCCalTargetMode   0x01102c04  0 最大 / 1 平均 / 2 指定  同左
 *   FFCCalTargetValue  0x01102c08  0..250（灰度）       × 16                       FFCCalTargetMode = 2（FFCCTModeAndEnableOnLocked）
 *   FFCCalFPN          0x01102084  1（命令：暗场校正，先遮光）   1                  见下（FFCFactoryLocked）
 *   FFCCalPRNU         0x01102088  1（命令：亮场校正，先打均匀光） 1                同上
 *   FFCUserSetLoad     0x01102d04  1（命令：把选中组的校正数据从闪存读回内存） 1    —
 *     FFCFactoryLocked：((0x010000b0 = 0 且 FFCUserSetSelector = 0) 或 ReverseX(0x01301000) = 1) 时锁定，
 *     也就是做校正前要先把 FFCUserSetSelector 选到 1..3。
 *   命令寄存器只能写（WO），没有读回；写完等相机重新应答。
 *
 * 注意 AnalogGain 的锁：XML 里 BinningV 和 BinningHorizontal 是同一个地址（0x01302040），不合并时值是 1，
 * 按 XML 的公式这时 AnalogGain 就是锁定的。我们照 XML 办（拒绝），不去试写；要调亮用 DigitalShift / DigitalGain / TDI。
 *
 * 只改运行时值：从不写 UserSetSave（0x01600318）、FFCUserSetSave（0x01102d08），相机断电恢复 UserSet1。
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
#define CAM16K_DIGITAL_SHIFT      0x01207040u
#define CAM16K_TDI_STAGE          0x01106008u
#define CAM16K_TDI_MODE           0x0110600cu
#define CAM16K_BLACK_LEVEL        0x0120c050u   /* 有符号 */
#define CAM16K_BLACK_LEVEL_MIN    0x0120c058u   /* 有符号 */
#define CAM16K_BLACK_LEVEL_MAX    0x0120c05cu
#define CAM16K_FFC_ENABLE         0x01102000u
#define CAM16K_FFC_SAMPLE_FRAMES  0x01102080u
#define CAM16K_FFC_CAL_FPN        0x01102084u   /* WO 命令 */
#define CAM16K_FFC_CAL_PRNU       0x01102088u   /* WO 命令 */
#define CAM16K_FFC_TARGET_MODE    0x01102c04u
#define CAM16K_FFC_TARGET_VALUE   0x01102c08u   /* 灰度 × 16 */
#define CAM16K_FFC_USER_SET       0x01102d00u
#define CAM16K_FFC_USER_SET_LOAD  0x01102d04u   /* WO 命令 */
#define CAM16K_FFC_FACTORY        0x010000b0u   /* RO */
#define CAM16K_REVERSE_X          0x01301000u
#define CAM16K_WIDTH              0x01303040u
#define CAM16K_MEASURED_PERIOD    0x01001038u   /* RO，实测行周期（时钟数） */
#define CAM16K_TEMPERATURE        0x01600328u   /* RO，℃ = (值 - 2000) / 10 */
#define CAM16K_MONO8              0x01080001u
#define CAM16K_SENSOR_MHZ         96u

/* 写之前从相机读到的状态（锁定条件和曝光范围要用）。 */
struct cam_set_state {
    uint32_t cl_config, pixel_format, binning_v, exposure_min_clk, exposure_max_clk;
    uint32_t line_period_min_clk, line_period_max_clk, line_trigger_mode;
    uint32_t ffc_factory, ffc_user_set, reverse_x, ffc_target_mode;
    int32_t black_min, black_max;
};

/* 写之前要从相机读的状态寄存器，顺序和 struct cam_set_state 的成员一一对应（全是 32 位）。 */
#define CAM_SET_STATE_COUNT 14
extern const uint32_t cam_set_state_addr[CAM_SET_STATE_COUNT];

/* 这个名字是不是"命令"（只写、值只能是 1、没有读回）。 */
int cam_set_is_command(const char *name);

/* 按 XML 的锁定公式：现在能不能做暗场 / 亮场校正。 */
int cam_set_ffc_cal_locked(const struct cam_set_state *st);

/* 名字 → 地址。不认识的名字返回 0。 */
uint32_t cam_set_address(const char *name);

/* 检查并换算：text 是命令行上的值。成功返回 0，*raw = 要写进寄存器的值；
 * 名字或格式不对 -EINVAL；值或当前状态不允许 -EPERM（why 指向原因说明）。 */
int cam_set_check(const char *name, const char *text, const struct cam_set_state *st,
                  uint32_t *raw, const char **why);

#endif
