/* SPDX-License-Identifier: MIT
 *
 * sg_cardxml.h —— 从采集卡存储里读出卡自己的 GenICam XML（SGCard.xml）。只读。
 *
 * 照 LusterGraber.dll（[逆向]，见 docs/vendor/reverse_engineering_2026-11-14.md 第 2 部分）：
 *   1. 第 N 路的 URL 字符串在逻辑地址 0x190 + N*0x100，长 0x100 字节（以 0 结尾）
 *   2. URL 形如 "Local:<文件名>;<十六进制地址>;<十六进制长度>"（"File:..." 表示 XML 在 Windows 本地）
 *   3. 再按地址和长度读出文件内容（可能是 .xml 或 .zip）
 * 全程只用卡邮箱的"读"，和预检同一条路径，旧驱动即可。
 */
#ifndef SG_CARDXML_H
#define SG_CARDXML_H
#include <stddef.h>
#include <stdint.h>
#include "sg_session.h"

#define SG_CARDXML_URL_ADDR(n)  (0x190u + (uint32_t)(n) * 0x100u)
#define SG_CARDXML_URL_LEN      0x100u
#define SG_CARDXML_MAX_LEN      (16u * 1024u * 1024u)

enum sg_cardxml_kind { SG_CARDXML_LOCAL = 0, SG_CARDXML_FILE = 1 };

/* 纯函数：解析 URL。Local → 0，File → 1（name 里是文件名，addr/len 不填），格式不对 → -EPROTO。
 * name 只保留字母、数字和 ._-，其余换成 _。 */
int sg_cardxml_parse_url(const char *url, char *name, size_t name_cap, uint32_t *addr, uint32_t *len);

/* 用邮箱读把 [addr, addr+len) 读进 buf，每次读 4 字节（和预检同一种读）。addr 要 4 字节对齐。 */
int sg_cardxml_read(struct sg_capture_session *s, uint32_t addr, void *buf, uint32_t len,
                    uint32_t timeout_ms);

#endif
