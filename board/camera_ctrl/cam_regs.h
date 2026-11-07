/* SPDX-License-Identifier: MIT
 *
 * cam_regs.h —— 相机（GenCP 设备）的寄存器地址。
 *
 * 来源：docs/vendor/genicam_xml_8k_reference/ 里凌云光 SF6200-CL08K-100M 的 GenICam XML。
 * ⚠️ 那是同系列的 **8K** 型号（XML 里 SensorWidth = 8192），不是我们的 16K 相机。
 *    同系列不同版本之间地址会变（例：CL 配置在 SF6100 XML 里是 0x01501028，这里是 0x01501008）。
 *    所以这里每一条都只标 [参考8K]，拿到 16K 相机自己的 XML 后逐条核对，核对过的改成 [16K确认]。
 *
 * 字节序：大端（BigEndian），32 位。
 * 换算：传感器时钟 SensorClockFrequency = 80 MHz
 *   曝光寄存器值 = 曝光时间(µs) × 80
 *   行频寄存器值 = 80,000,000 / 行频(Hz)       例：15,360 Hz → 5208
 */
#ifndef CAM_REGS_H
#define CAM_REGS_H

/* 图像尺寸 */
#define CAM_REG_WIDTH_MAX          0x01303038u  /* RO [参考8K] */
#define CAM_REG_WIDTH              0x01303040u  /* RW [参考8K] 3 份 XML 地址一致 */
#define CAM_REG_HEIGHT             0x01303044u  /* RW [参考8K] 3 份 XML 地址一致 */
#define CAM_REG_PIXEL_FORMAT       0x01401040u  /* RW [参考8K] Mono8 = 0x01080001 */

/* 曝光与行频 */
#define CAM_REG_EXPOSURE_MODE      0x01001080u  /* RW [参考8K] Timed = 1，TriggerWidth = 0（注意：Timed 不是 0）*/
#define CAM_REG_EXPOSURE_TIME_MODE 0x01001084u  /* RW [参考8K] */
#define CAM_REG_EXPOSURE_CLK       0x01001010u  /* RW [参考8K] 单位：80 MHz 时钟数 */
#define CAM_REG_LINE_PERIOD_CLK    0x0100101Cu  /* RW [参考8K] 单位：80 MHz 时钟数 */

/* 触发（接编码器后用）*/
#define CAM_REG_LINE_TRIG_MODE     0x01001004u  /* RW [参考8K] 0 关（内部行频）/ 1 开 */
#define CAM_REG_LINE_TRIG_SOURCE   0x01006044u  /* RW [参考8K] */
#define CAM_REG_LINE_TRIG_EDGE     0x01006048u  /* RW [参考8K] */

/* 测试图案（verify/V1 用）*/
#define CAM_REG_TEST_PATTERN       0x01100000u  /* RW [参考8K] 0 关 1 黑 2 白 3 水平灰度渐变 4 垂直渐变 5/6 移动渐变 */

/* Camera Link 输出配置 */
#define CAM_REG_CL_CONFIG          0x01501008u  /* RW [参考8K] 只有两种：Base = 0，EightyBit（双线）= 3 */
#define CAM_REG_CL_TAPS            0x01501010u  /* RW [参考8K] 1 / 2 / 3 / 10 tap */
#define CAM_REG_CL_CLOCK           0x01501060u  /* RW [参考8K] 85 / 80 / 66 / 40（MHz）*/

/* 串口与配置保存 */
#define CAM_REG_BAUD               0x00000004u  /* RW [参考8K] 0x01 = 9600，0x10 = 115200，0x40 = 460800 */
#define CAM_REG_USERSET_SELECTOR   0x01600310u  /* RW [参考8K] */
#define CAM_REG_USERSET_LOAD       0x01600314u  /* WO [参考8K] */
#define CAM_REG_USERSET_SAVE       0x01600318u  /* WO [参考8K] 外部报告写的 0x01000000 是错的 */
#define CAM_REG_USERSET_DEFAULT    0x01600320u  /* RW [参考8K] 上电默认加载哪个 User Set */
#define CAM_REG_TEMPERATURE        0x01600328u  /* RO [参考8K] */

#endif
