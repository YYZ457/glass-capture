/* SPDX-License-Identifier: MIT
 *
 * cam_serial.h —— Camera Link 串口通道（相机 ⇄ 采集卡 ⇄ 主机）。
 *
 * Camera Link 线缆里有一对串口线，相机的所有配置命令都走这里，由采集卡转发。
 * 卡上的串口邮箱和卡邮箱（sg_mailbox）同构，地址见 sg3000_regs.h 的 SG_SER_*（[逆向]）。
 *
 * 分三层，状态不同：
 *   1. 字节收发 cam_serial_send / recv     🟡 已写，假卡测过；需要新驱动放开白名单才能上板
 *   2. 波特率   cam_serial_set_baud         ⬜ -ENOSYS：分频寄存器在哪个 BAR 没确认
 *   3. GenCP    cam_gencp_read32 / write32  ⬜ -ENOSYS：串口上 GenCP 帧的校验算法没确认
 * 详见 docs/interfaces/I7_camera_serial.md。
 *
 * 短期替代：在 Windows 上用 VisionKit 配好并保存到相机 User Set，本模块可延后。
 */
#ifndef CAM_SERIAL_H
#define CAM_SERIAL_H
#include <stddef.h>
#include <stdint.h>
#include "sg_session.h"

/* 一包载荷上限。卡的 FIFO 深度未知，先取保守值；GenCP 读写一个寄存器远小于它。 */
#define CAM_SERIAL_MAX_PAYLOAD 512u
#define CAM_SERIAL_HDR_BYTES   16u
#define CAM_SERIAL_MAX_WORDS   ((CAM_SERIAL_HDR_BYTES + CAM_SERIAL_MAX_PAYLOAD + 3u) / 4u)

struct cam_serial {
    struct sg_capture_session *s;
    unsigned port;          /* 0 或 1（Camera Link 串口 0 / 1）*/
    uint16_t seq;           /* 发送序号，每发一包 +1 */
};

/* port 只能是 0 或 1，否则 -EINVAL。 */
int cam_serial_init(struct cam_serial *c, struct sg_capture_session *s, unsigned port);

/* 纯计算：16550 分频值 = round(200 MHz / (16 * baud))。
 * 只接受 9600/19200/38400/57600/115200/230400/460800/921600，否则 -EINVAL。
 * 注意 921600 实际误差 -3.1%，不建议用。 */
int cam_uart_divisor(uint32_t baud, uint32_t *divisor);

/* 纯计算：组一包（16 字节头 + 载荷，按小端 32 位字，最后一字不足补 0）。
 * 成功返回字数，参数不对返回 -EINVAL，words 放不下返回 -ENOSPC。 */
int cam_serial_pack(unsigned port, uint16_t seq, const void *payload, uint32_t len,
                    uint32_t *words, size_t capacity);

/* 纯计算：看回包的 16 字节头，得出载荷长度。
 * 头第 0 字低 16 位为 0x55aa 时取头偏移 8 处的 u16，否则取 total - 16。
 * total < 16 或载荷长度超出 total - 16 → -EPROTO。 */
int cam_serial_payload_len(const uint32_t header[4], uint32_t total, uint32_t *len);

/* 发一包：逐字写 TX_DATA，再写 TX_LEN。len 为 1..CAM_SERIAL_MAX_PAYLOAD。 */
int cam_serial_send(struct cam_serial *c, const void *payload, uint32_t len);

/* 等一包回包（最多 timeout_ms），把载荷拷进 buf。
 * 超时 -ETIMEDOUT；回包比 capacity 大 → 整包读完（不留在 FIFO 里）后返回 -EMSGSIZE。 */
int cam_serial_recv(struct cam_serial *c, void *buf, uint32_t capacity, uint32_t *len,
                    uint32_t timeout_ms);

/* 设波特率。⬜ 目前恒返回 -ENOSYS（见文件头）。 */
int cam_serial_set_baud(struct cam_serial *c, uint32_t baud);

/* GenCP 读写一个 32 位寄存器。⬜ 目前恒返回 -ENOSYS（见文件头）。 */
int cam_gencp_read32(struct sg_capture_session *s, uint32_t addr, uint32_t *value, uint32_t timeout_ms);
int cam_gencp_write32(struct sg_capture_session *s, uint32_t addr, uint32_t value, uint32_t timeout_ms);

#endif
