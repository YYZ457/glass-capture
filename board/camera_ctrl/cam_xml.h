/* SPDX-License-Identifier: MIT
 *
 * cam_xml.h —— 经 GenCP 从相机本身读出它的 GenICam XML（不需要 Windows）。
 *
 * 这是 GenCP 标准的引导流程，VisionKitApi.dll 读"设备 XML"走的也是这一条（[逆向]，
 * 0x1800469b0..0x180047176，见 docs/interfaces/I7 第 9 节）：
 *
 *   1. 读 BRM 0x01D0（8 字节，大端）= 清单表地址 T
 *   2. 读 T（8 字节，大端）           = 清单条目数
 *   3. 读 T + 8 + 64·i（64 字节）     = 第 i 条：文件版本、格式版本、XML 地址、XML 大小、SHA1
 *   4. 格式版本 bit10..15：0 = 未压缩 .xml，1 = .zip
 *   5. 从 XML 地址按块读出 XML 大小那么多字节
 *
 * 全部只读（GenCP ReadMem），不写相机。
 */
#ifndef CAM_XML_H
#define CAM_XML_H
#include <stddef.h>
#include <stdint.h>
#include "cam_serial.h"

/* GenCP 标准 BRM（Technology Agnostic Bootstrap Register Map）[GenCP 标准]。
 * 0x0004 / 0x0044 已在实板读出 "JAI" / "SW-WS2-CL16K-50M-SDE"。 */
#define CAM_BRM_GENCP_VERSION   0x0000u   /* 4 字节 */
#define CAM_BRM_MANUFACTURER    0x0004u   /* 64 字节字符串 */
#define CAM_BRM_MODEL           0x0044u
#define CAM_BRM_FAMILY          0x0084u
#define CAM_BRM_DEVICE_VERSION  0x00c4u
#define CAM_BRM_MANUFACTURER_INFO 0x0104u
#define CAM_BRM_SERIAL          0x0144u
#define CAM_BRM_USER_NAME       0x0184u
#define CAM_BRM_CAPABILITY      0x01c4u   /* 8 字节 */
#define CAM_BRM_MAX_RESPONSE_MS 0x01ccu   /* 4 字节 */
#define CAM_BRM_MANIFEST_TABLE  0x01d0u   /* 8 字节：清单表地址 */
#define CAM_BRM_SBRM_ADDRESS    0x01d8u   /* 8 字节：SBRM 地址 */
#define CAM_BRM_STRING_LEN      64u

#define CAM_MANIFEST_ENTRY_BYTES 64u
#define CAM_MANIFEST_MAX_ENTRIES 8u
#define CAM_XML_MAX_BYTES        (8u * 1024u * 1024u)   /* 正常相机 XML 几十 KB 到 1 MB */

struct cam_manifest_entry {
    uint32_t file_version;    /* bit24..31 主、bit16..23 次、bit0..15 小版本 */
    uint32_t schema_version;  /* bit10..15 文件类型（0 xml / 1 zip）*/
    uint64_t address;         /* XML 在相机里的起始地址 */
    uint64_t size;            /* 字节数 */
    unsigned char sha1[20];   /* 全 0 = 相机没给 */
};

/* 纯函数：大端 64 位。 */
uint64_t cam_be64(const unsigned char *p);

/* 纯函数：解析一条 64 字节的清单条目。文件类型不是 0/1、大小为 0 或超过
 * CAM_XML_MAX_BYTES、地址+大小溢出 → -EPROTO。 */
int cam_manifest_parse(const unsigned char b[CAM_MANIFEST_ENTRY_BYTES], struct cam_manifest_entry *e);

/* 纯函数：".xml" / ".zip"；类型未知返回 NULL。 */
const char *cam_manifest_ext(const struct cam_manifest_entry *e);

/* 纯函数：SHA1 是否全 0。 */
int cam_manifest_has_sha1(const struct cam_manifest_entry *e);

/* 进度回调：done / total 字节。可以为 NULL。 */
typedef void (*cam_progress_fn)(uint64_t done, uint64_t total, void *ctx);

/* 按块（CAM_GENCP_MAX_DATA）读任意长度。每块失败（超时 / 包错）最多重试 retries 次，
 * 重试前清掉串口里迟到的字节。相机回非 0 状态码（-EREMOTEIO）不重试。 */
int cam_read_mem(struct cam_serial *c, uint64_t addr, void *buf, uint64_t len,
                 uint32_t timeout_ms, unsigned retries, cam_progress_fn progress, void *ctx);

/* 读清单表：*table = 表地址，entries 最多 cap 条，*count = 相机报的条目数（可能 > cap，
 * 此时只解析前 cap 条）。条目数为 0 或 > 255 → -EPROTO。 */
int cam_manifest_read(struct cam_serial *c, uint64_t *table, struct cam_manifest_entry *entries,
                      unsigned cap, unsigned *count, uint32_t timeout_ms);

/* 把串口里残留的回包读掉（最多 max_packets 包，每包等 wait_ms）。返回丢掉的包数。 */
int cam_serial_drain(struct cam_serial *c, unsigned max_packets, uint32_t wait_ms);

#endif
