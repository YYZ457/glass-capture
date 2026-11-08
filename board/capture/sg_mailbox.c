/* SPDX-License-Identifier: MIT
 *
 * sg_mailbox.c —— 邮箱报文的组装、发送、等待、解析。
 * 职责：只管"把一条命令送到卡固件并拿回答复"，不知道任何逻辑地址的含义。
 * 依赖：sg_session（rd/wr/now/sleep）、sg3000_regs.h 的 SG_MBX_*
 * 被谁用：sg_preflight.c、sg_acq.c
 *
 * 迁移来源：legacy sg3000_capture_core.c
 *   le16 / le32 / put32                 ← 第 13-17 行（字节序小工具，可放本文件 static）
 *   logical()                           ← 第 39-83 行，拆成下面两个对外函数
 * 怎么验证：
 *   1. 电脑上用假回调表喂一段录下来的回复，检查解析出的值
 *   2. 板上连读两次 SG_L_DDR_STATUS，都应是 2，且 ACK 计数连续
 *
 * 一次完整的往返分四步（对应下面四个 static 函数）：
 *   mbx_send        把请求写进发送窗口
 *   mbx_wait_reply  轮询，等回复窗口里出现数据
 *   mbx_fetch_reply 把回复逐字读出来，存成字节数组
 *   mbx_parse_reply 逐项核对回复；读操作时取出值
 *
 * 原来 legacy 的 logical() 是一个 45 行的大函数。这里拆开只是为了好读，
 * 每一步里的判断、顺序、返回的错误码都和 legacy 完全一致。
 *
 * 安全约束（不要放宽）：
 *   - 写只允许 SG_L_ACQ_START / SG_L_ACQ_STOP，其他返回 -EPERM
 *   - 发送前回复窗口非空 → -EBUSY，不清理、不重试
 */
#include <string.h>

#include "sg_mailbox.h"
#include "sg3000_regs.h"
#include "sg_err.h"

/* 回复最长不会超过 32 字节，多留一些，只是防止卡返回异常长度时越界。 */
#define MBX_REPLY_MAX   600u

/* ---- 小端字节序小工具 ------------------------------------------------ */
/* 回复是一串字节。小端：低位字节放在前面。 */

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const unsigned char *p)
{
    return  (uint32_t)p[0]
          | ((uint32_t)p[1] << 8)
          | ((uint32_t)p[2] << 16)
          | ((uint32_t)p[3] << 24);
}

/* 反方向：把一个 32 位数拆成 4 个字节（低位在前）写进 p。 */
static void put32(unsigned char *p, uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4; ++i) {
        p[i] = (unsigned char)(value >> (8 * i));
    }
}

/* ---- 第 1 步：发送 --------------------------------------------------- */

static int mbx_send(struct sg_capture_session *s, uint32_t address, int write)
{
    /* 报文共 7 个字（读）或 8 个字（写）。下标含义见 sg_mailbox.h 顶部注释。
     * 先填固定的字，其余几个字下面再填。 */
    uint32_t packet[8] = { SG_MBX_REQ_MAGIC, 0, 0, 0, 0, 0, 4, 1 };
    unsigned words = write ? 8u : 7u;
    uint32_t occupancy, room;
    unsigned i;
    int ret;

    /* 回复窗口必须是空的，否则说明有旧回复没取走。不清理、不重试，直接报错。 */
    ret = sg_session_rd(s, SG_MBX_RX_OCCUPANCY, &occupancy);
    if (ret) {
        return ret;
    }
    if (occupancy) {
        return -EBUSY;
    }

    /* 发送窗口要有足够空间。 */
    ret = sg_session_rd(s, SG_MBX_TX_ROOM, &room);
    if (ret) {
        return ret;
    }
    if (room < words) {
        return -ENOSPC;
    }

    /* 第 2 个字：长度（读 12 / 写 16）放低 16 位，请求号放高 16 位。
     * 请求号每发一条先加 1。 */
    s->request_id++;
    packet[2] = (write ? 16u : 12u) | ((uint32_t)s->request_id << 16);
    packet[4] = write ? SG_MBX_OP_WRITE : SG_MBX_OP_READ;
    packet[5] = address;

    /* 逐字写进发送窗口，最后写一次"总字节数"，卡才会把报文发出去。 */
    for (i = 0; i < words; ++i) {
        ret = sg_session_wr(s, SG_MBX_TX_DATA, packet[i]);
        if (ret) {
            return ret;
        }
    }
    return sg_session_wr(s, SG_MBX_TX_LEN, words * 4u);
}

/* ---- 第 2 步：等回复 ------------------------------------------------- */

static int mbx_wait_reply(struct sg_capture_session *s, uint64_t deadline, int cleanup)
{
    uint32_t occupancy;
    int ret;

    for (;;) {
        /* 停止采集（cleanup）时不理会 Ctrl+C，但仍然受超时限制。 */
        if (!cleanup && sg_session_cancelled(s)) {
            return -ECANCELED;
        }
        if (s->io.now_ms(s->io.context) >= deadline) {
            return -ETIMEDOUT;
        }

        ret = sg_session_rd(s, SG_MBX_RX_OCCUPANCY, &occupancy);
        if (ret) {
            return ret;
        }
        if (occupancy) {
            return 0;                       /* 回复到了 */
        }
        s->io.sleep_ms(s->io.context, 1);   /* 没到，睡 1 毫秒再看 */
    }
}

/* ---- 第 3 步：取回复 ------------------------------------------------- */

static int mbx_fetch_reply(struct sg_capture_session *s, unsigned char *reply,
                           uint32_t *length_out)
{
    uint32_t length, word, i;
    int ret;

    ret = sg_session_rd(s, SG_MBX_RX_LEN, &length);
    if (ret) {
        return ret;
    }
    /* 至少 28 字节、不超过缓冲区、必须是 4 的倍数（按字读）。 */
    if (length < 28 || length > MBX_REPLY_MAX || (length & 3u)) {
        return -EPROTO;
    }

    for (i = 0; i < length; i += 4) {
        ret = sg_session_rd(s, SG_MBX_RX_DATA, &word);
        if (ret) {
            return ret;
        }
        put32(reply + i, word);             /* 一个字拆成 4 个字节存进数组 */
    }

    *length_out = length;
    return 0;
}

/* ---- 第 4 步：核对回复 ----------------------------------------------- */

static int mbx_parse_reply(struct sg_capture_session *s, const unsigned char *reply,
                           uint32_t length, int write, uint32_t *value)
{
    uint16_t ack;

    /* 这些字段都来自观察，不是厂家文档。任何一项对不上，就说明对协议的理解有误。 */
    if (length != (write ? 28u : 32u) ||
        le32(reply) != SG_MBX_ACK_MAGIC ||
        le32(reply + 4) != 1u ||
        le16(reply + 8) != (write ? 12u : 16u) ||
        le16(reply + 16) != 0 ||
        le16(reply + 18) != (write ? SG_MBX_ACK_WRITE : SG_MBX_ACK_READ) ||
        le16(reply + 24) != (write ? 0u : 4u)) {
        return -EPROTO;
    }

    /* 回复偏移 10 处是 ACK 计数。它和请求号是两个独立的计数器，
     * 不要求回复里原样返回请求号；但同一个会话里必须正好 +1。 */
    ack = le16(reply + 10);
    if (s->have_ack && ack != (uint16_t)(s->last_ack + 1u)) {
        return -EPROTO;
    }
    s->have_ack = 1;
    s->last_ack = ack;

    if (!write) {
        *value = le32(reply + 28);          /* 读操作：值在偏移 28 */
    }
    return 0;
}

/* ---- 读写共用的主流程 ------------------------------------------------ */

static int mbx_transact(struct sg_capture_session *s, uint32_t address, int write,
                        uint32_t *value, uint32_t timeout_ms, int cleanup)
{
    unsigned char reply[MBX_REPLY_MAX];
    uint32_t length = 0;
    uint64_t deadline = s->io.now_ms(s->io.context) + timeout_ms;
    int ret;

    if (!cleanup && sg_session_cancelled(s)) {
        return -ECANCELED;
    }
    /* 白名单：写只允许开始 / 停止采集。 */
    if (write && address != SG_L_ACQ_START && address != SG_L_ACQ_STOP) {
        return -EPERM;
    }

    ret = mbx_send(s, address, write);
    if (ret) {
        return ret;
    }
    ret = mbx_wait_reply(s, deadline, cleanup);
    if (ret) {
        return ret;
    }
    ret = mbx_fetch_reply(s, reply, &length);
    if (ret) {
        return ret;
    }
    return mbx_parse_reply(s, reply, length, write, value);
}

/* ---- 对外的两个函数 -------------------------------------------------- */

int sg_mbx_read(struct sg_capture_session *s, uint32_t logical_addr, uint32_t *value,
                uint32_t timeout_ms, int cleanup)
{
    if (s == NULL || value == NULL) {
        return -EINVAL;
    }
    return mbx_transact(s, logical_addr, 0, value, timeout_ms, cleanup);
}

int sg_mbx_write(struct sg_capture_session *s, uint32_t logical_addr,
                 uint32_t timeout_ms, int cleanup)
{
    if (s == NULL) {
        return -EINVAL;
    }
    return mbx_transact(s, logical_addr, 1, NULL, timeout_ms, cleanup);
}
