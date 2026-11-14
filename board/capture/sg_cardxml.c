/* SPDX-License-Identifier: MIT
 *
 * sg_cardxml.c —— 见 sg_cardxml.h。
 */
#include <stdlib.h>
#include <string.h>

#include "sg_cardxml.h"
#include "sg_mailbox.h"
#include "sg_err.h"

static int parse_hex(const char *p, const char **end, uint32_t *out)
{
    uint64_t v = 0;
    const char *q = p;

    while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F')) {
        v = v * 16u + (uint64_t)(*q <= '9' ? *q - '0' : (*q | 0x20) - 'a' + 10);
        if (v > 0xffffffffu) return -EPROTO;
        ++q;
    }
    if (q == p) return -EPROTO;
    *end = q;
    *out = (uint32_t)v;
    return 0;
}

static void copy_name(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i;

    if (n >= cap) n = cap - 1;
    for (i = 0; i < n; ++i) {
        char ch = src[i];
        int ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 ch == '.' || ch == '_' || ch == '-';
        dst[i] = ok ? ch : '_';
    }
    dst[n] = 0;
}

int sg_cardxml_parse_url(const char *url, char *name, size_t name_cap, uint32_t *addr, uint32_t *len)
{
    const char *p, *semi;

    if (!url || !name || name_cap < 2 || !addr || !len) return -EINVAL;
    if (!strncmp(url, "File:", 5)) {
        p = url + 5;
        copy_name(name, name_cap, p, strcspn(p, "?"));
        return name[0] ? SG_CARDXML_FILE : -EPROTO;
    }
    if (strncmp(url, "Local:", 6)) return -EPROTO;
    p = url + 6;
    semi = strchr(p, ';');
    if (!semi || semi == p) return -EPROTO;
    copy_name(name, name_cap, p, (size_t)(semi - p));
    if (parse_hex(semi + 1, &p, addr) || *p != ';' || parse_hex(p + 1, &p, len)) return -EPROTO;
    if (*p && *p != '?') return -EPROTO;
    if (*len == 0 || *len > SG_CARDXML_MAX_LEN) return -EPROTO;
    return SG_CARDXML_LOCAL;
}

int sg_cardxml_read(struct sg_capture_session *s, uint32_t addr, void *buf, uint32_t len,
                    uint32_t timeout_ms)
{
    unsigned char *out = buf, blk[SG_MBX_BLOCK_MAX];
    uint32_t done = 0, n;
    int ret;

    if (!s || !buf || !len || (addr & 3u) || (uint64_t)addr + len > 0x100000000ull) return -EINVAL;
    /* 照实板/Windows：存储块读每包固定请求 0x100 字节；实板对不足 0x100 的请求也回 0x100 字节。
     * 最后一包若不足 0x100 字节，先读进临时缓冲再截断拷贝，避免回包长度校验误报 -EPROTO。 */
    while (done < len) {
        n = len - done;
        if (n >= SG_MBX_BLOCK_MAX) {
            ret = sg_mbx_read_block(s, addr + done, out + done, SG_MBX_BLOCK_MAX, timeout_ms);
            if (ret) return ret;
            done += SG_MBX_BLOCK_MAX;
        } else {
            ret = sg_mbx_read_block(s, addr + done, blk, SG_MBX_BLOCK_MAX, timeout_ms);
            if (ret) return ret;
            memcpy(out + done, blk, n);
            done += n;
        }
    }
    return 0;
}
