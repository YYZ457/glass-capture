/* SPDX-License-Identifier: MIT
 *
 * cam_regs.h —— 相机（GenCP 设备）的寄存器地址。
 *
 * 来源：最初抄自同系列 8K 型号的 XML。2026-11-14 从我们这台 16K 相机（SW-WS2-CL16K-50M-SDE，
 *   固件 300.001.22）本身读出了 XML（docs/vendor/camera_xml/），逐条核对：除串口波特率外地址全部一致，
 *   已改标 [16K确认]。波特率寄存器原来写的 0x4 是错的（那是 BRM 厂商名），已更正。
 *
 * 字节序：大端（BigEndian），32 位。
 * 换算：传感器时钟 SensorClockFrequency = 96 MHz（16K XML；8K 型号是 80）
 *   曝光寄存器值 = 曝光时间(µs) × 96              实测 4800 = 50 µs
 *   行频寄存器值 = 96,000,000 / 行频(Hz)           实测 9600 = 10 kHz（与采集卡测得的行频一致）
 */
#ifndef CAM_REGS_H
#define CAM_REGS_H

/* 图像尺寸 */
#define CAM_REG_WIDTH_MAX          0x01303038u  /* RO [16K确认] */
#define CAM_REG_WIDTH              0x01303040u  /* RW [16K确认] 3 份 XML 地址一致 */
#define CAM_REG_HEIGHT             0x01303044u  /* RW [16K确认] 3 份 XML 地址一致 */
#define CAM_REG_PIXEL_FORMAT       0x01401040u  /* RW [16K确认] Mono8 = 0x01080001 */

/* 曝光与行频 */
#define CAM_REG_EXPOSURE_MODE      0x01001080u  /* RW [16K确认] Timed = 1，TriggerWidth = 0（注意：Timed 不是 0）*/
#define CAM_REG_EXPOSURE_TIME_MODE 0x01001084u  /* RW [16K确认] */
#define CAM_REG_EXPOSURE_CLK       0x01001010u  /* RW [16K确认] 单位：96 MHz 时钟数 */
#define CAM_REG_LINE_PERIOD_CLK    0x0100101Cu  /* RW [16K确认] 单位：96 MHz 时钟数 */

/* 触发（接编码器后用）*/
#define CAM_REG_LINE_TRIG_MODE     0x01001004u  /* RW [16K确认] 0 关（内部行频）/ 1 开 */
#define CAM_REG_LINE_TRIG_SOURCE   0x01006044u  /* RW [16K确认] */
#define CAM_REG_LINE_TRIG_EDGE     0x01006048u  /* RW [16K确认] */

/* 测试图案（verify/V1 用）*/
#define CAM_REG_TEST_PATTERN       0x01100000u  /* RW [16K确认] 0 Off / 1 Black / 2 White / 3 GreyHorizontalRamp / 5 GreyHorizontalRampMoving（16K XML 没有 4、6；那是 8K 的） */

/* Camera Link 输出配置 */
#define CAM_REG_CL_CONFIG          0x01501008u  /* RW [16K确认] 只有两种：Base = 0，EightyBit（双线）= 3；实测 3 */
#define CAM_REG_CL_TAPS            0x01501010u  /* RW [16K确认] Base 时 2 / 3，EightyBit 时 10（锁定）；实测 10 */
#define CAM_REG_CL_CLOCK           0x01501060u  /* RW [16K确认] 85 / 80 / 65 / 40（MHz），实测 85*/

/* 串口与配置保存 */
#define CAM_REG_BAUD               0x00010004u  /* RW [16K确认] = SBRM（BRM 0x1D8，实测 0x10000）+ 0x04；0x01 = 9600，0x10 = 115200，0x40 = 460800 */
#define CAM_REG_USERSET_SELECTOR   0x01600310u  /* RW [16K确认] */
#define CAM_REG_USERSET_LOAD       0x01600314u  /* WO [16K确认] */
#define CAM_REG_USERSET_SAVE       0x01600318u  /* WO [16K确认] 外部报告写的 0x01000000 是错的 */
#define CAM_REG_USERSET_DEFAULT    0x01600320u  /* RW [16K确认] 上电默认加载哪个 User Set */
#define CAM_REG_TEMPERATURE        0x01600328u  /* RO [16K确认] */

#endif
