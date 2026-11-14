/* SPDX-License-Identifier: MIT
 *
 * cam_serial.h —— Camera Link 串口通道（相机 ⇄ 采集卡 ⇄ 主机）和上面的 GenCP。
 *
 * 全部照 Windows 驱动 XDMA.sys 和 VisionKitApi.dll 的反汇编（[逆向]），出处见
 * docs/vendor/reverse_engineering_2026-11-14.md 第 2 部分。
 *
 *   1. 串口邮箱  cam_serial_open / send / recv / set_baud   卡上 0x4110xx / 0x4130xx
 *   2. GenCP     cam_gencp_read / write（read32 / write32） 相机寄存器按地址读写
 *
 * 🟡 假卡测过，没上过板。需要新驱动放开白名单（旧 .ko 会拒绝，返回 -EINVAL，碰不到硬件）。
 */
#ifndef CAM_SERIAL_H
#define CAM_SERIAL_H
#include <stddef.h>
#include <stdint.h>
#include "sg_session.h"

/* 一包串口邮箱载荷的上限。卡的 FIFO 深度未知，先取保守值；GenCP 读写寄存器远小于它。 */
#define CAM_SERIAL_MAX_PAYLOAD 512u
#define CAM_SERIAL_HDR_BYTES   16u
#define CAM_SERIAL_MAX_WORDS   ((CAM_SERIAL_HDR_BYTES + CAM_SERIAL_MAX_PAYLOAD + 3u) / 4u)

/* GenCP 串口包：8 字节前缀 + 8 字节 CCD + SCD。一次读写的数据上限（取保守值）。 */
#define CAM_GENCP_HDR_BYTES    16u
#define CAM_GENCP_MAX_DATA     256u
#define CAM_GENCP_RX_CAP       (CAM_GENCP_HDR_BYTES + CAM_GENCP_MAX_DATA + CAM_SERIAL_MAX_PAYLOAD)

struct cam_serial {
    struct sg_capture_session *s;
    unsigned port;          /* 0 或 1（Camera Link 串口 0 / 1）*/
    uint16_t seq;           /* 串口邮箱序号：第一包是 0，之后 +1（照 XDMA.sys）*/
    uint16_t gencp_req;     /* GenCP 请求号：从 1 起，跳过 0 */
    unsigned char rx[CAM_GENCP_RX_CAP];  /* 串口收到、还没拼成完整 GenCP 包的字节 */
    size_t rx_len;
};

/* ---- 第 1 层：串口邮箱 ---- */

/* 只填结构体，不碰硬件。port 只能是 0 或 1，否则 -EINVAL。 */
int cam_serial_init(struct cam_serial *c, struct sg_capture_session *s, unsigned port);

/* 照 Windows 打开 \data_N 时的初始化：读 +0x04，不等于 0x04000000 时
 * 往 +0x28/+0x18/+0x08 写 0xa5 并读 +0x1c，最多 3 轮直到 +0x1c 为 0，再写 +0x04 = 0x04000000。 */
int cam_serial_open(struct cam_serial *c, struct sg_capture_session *s, unsigned port);

/* 纯计算：16550 分频值 = (200 MHz + 8·baud) / (16·baud)。
 * 只接受 9600/19200/38400/57600/115200/230400/460800/921600，否则 -EINVAL。
 * 921600 实际误差 -3.1%，不建议用。 */
int cam_uart_divisor(uint32_t baud, uint32_t *divisor);

/* 纯计算：组一包（16 字节头 + 载荷，按小端 32 位字，最后一字不足补 0）。
 * 成功返回字数，参数不对 -EINVAL，words 放不下 -ENOSPC。 */
int cam_serial_pack(unsigned port, uint16_t seq, const void *payload, uint32_t len,
                    uint32_t *words, size_t capacity);

/* 纯计算：回包头第 0 字低 16 位为 0x55aa 时载荷长度取头偏移 8 的 u16，否则 total - 16。
 * total < 16 或载荷超出 total - 16 → -EPROTO。 */
int cam_serial_payload_len(const uint32_t header[4], uint32_t total, uint32_t *len);

/* 发一包：逐字写 TX_DATA，再写 TX_LEN = len + 16。len 为 1..CAM_SERIAL_MAX_PAYLOAD。 */
int cam_serial_send(struct cam_serial *c, const void *payload, uint32_t len);

/* 等一包（最多 timeout_ms），载荷拷进 buf。读 ceil(载荷/4) 个字（照 XDMA.sys）。
 * 超时 -ETIMEDOUT；回包比 capacity 大 → 读完后返回 -EMSGSIZE（*len 给出实际长度）。 */
int cam_serial_recv(struct cam_serial *c, void *buf, uint32_t capacity, uint32_t *len,
                    uint32_t timeout_ms);

/* 设波特率（照 XDMA.sys IOCTL 0x220024 的写入顺序）。UART 在 BAR0 的 (port<<16)+0x1000。 */
int cam_serial_set_baud(struct cam_serial *c, uint32_t baud);

/* ---- 第 2 层：GenCP ---- */

/* 纯计算：16 位反码求和（大端 16 位字，奇数长度最后一字节放高位），取反。 */
uint16_t cam_gencp_checksum(const unsigned char *p, size_t n);

/* 纯计算：组 ReadMem（28 字节）/ WriteMem（24 + len 字节）包。返回字节数或 -EINVAL/-ENOSPC。 */
int cam_gencp_build_read(uint16_t req_id, uint64_t addr, uint16_t len,
                         unsigned char *out, size_t capacity);
int cam_gencp_build_write(uint16_t req_id, uint64_t addr, const void *data, uint16_t len,
                          unsigned char *out, size_t capacity);

/* 按地址读写相机内存（字节原样，不做字节序转换）。len 为 1..CAM_GENCP_MAX_DATA。
 * 相机回 PendingAck 时按它给的时间延长等待。错误：
 *   -ETIMEDOUT 没回应；-EPROTO 包格式 / 请求号不对；-EREMOTEIO 相机回了非 0 状态码。 */
int cam_gencp_read(struct cam_serial *c, uint64_t addr, void *buf, uint16_t len, uint32_t timeout_ms);
int cam_gencp_write(struct cam_serial *c, uint64_t addr, const void *data, uint16_t len, uint32_t timeout_ms);

/* 读写一个 32 位寄存器（相机 XML 规定大端）。 */
int cam_gencp_read32(struct cam_serial *c, uint32_t addr, uint32_t *value, uint32_t timeout_ms);
int cam_gencp_write32(struct cam_serial *c, uint32_t addr, uint32_t value, uint32_t timeout_ms);

#endif
