/* SPDX-License-Identifier: MIT
 *
 * sg3000_regs.h —— SG3000 采集卡的所有地址和魔数，**只在这里定义一次**。
 *
 * 可信度标记（每一条都必须有）：
 *   [厂家]  来自厂家文档                      —— 目前没有任何一条
 *   [观察]  来自观察 Windows 驱动 / VisionKit 的行为，并在 ZCU102 上实测通过
 *   [推测]  能用，但含义是推断出来的
 *   [未知]  只知道要读/写它，不知道它是什么
 *
 * 地址分三类：
 *   A. BAR0 寄存器：CPU 直接读写（经驱动的 SG_IOC_READ_REG / WRITE_REG）
 *   B. BAR1 XDMA 引擎：只有内核驱动碰
 *   C. 逻辑地址：不能直接读写，要经 A 里的邮箱发命令报文，由卡上固件代为读写
 *
 * 迁移来源：legacy/sg3000-core-v0.2/sg3000_capture_core.c、sg3000_rx.c
 * 实测数值来自 2026-10-30 的板上日志。
 */
#ifndef SG3000_REGS_H
#define SG3000_REGS_H

/* ===== PCIe 身份 ===================================================== */
#define SG_PCI_VENDOR            0x4c54u      /* [观察] 实测识别为 4c54:3000 */
#define SG_PCI_DEVICE            0x3000u
#define SG_BAR0_LEN              0x1000000u   /* [观察] 16 MiB，卡的寄存器窗口 */
#define SG_BAR1_LEN              0x10000u     /* [观察] 64 KiB，XDMA 引擎 */

/* ===== A. BAR0：邮箱（给卡上固件发命令）=============================== */
#define SG_MBX_BASE              0x412000u
#define SG_MBX_TX_ROOM           (SG_MBX_BASE + 0x0c) /* 读 [推测] 发送 FIFO 剩余空间（字） */
#define SG_MBX_TX_DATA           (SG_MBX_BASE + 0x10) /* 写 [观察] 逐字写入请求报文 */
#define SG_MBX_TX_LEN            (SG_MBX_BASE + 0x14) /* 写 [观察] 报文字节数，只允许 28 或 32 */
#define SG_MBX_RX_OCCUPANCY      (SG_MBX_BASE + 0x1c) /* 读 [推测] 非 0 = 有回复待取 */
#define SG_MBX_RX_DATA           (SG_MBX_BASE + 0x20) /* 读 [观察] 逐字读出回复 */
#define SG_MBX_RX_LEN            (SG_MBX_BASE + 0x24) /* 读 [观察] 回复字节数 */

#define SG_MBX_REQ_MAGIC         0x000155aau  /* [观察] 请求报文第 0 字 */
#define SG_MBX_ACK_MAGIC         0x000055aau  /* [观察] 回复报文第 0 字 */
#define SG_MBX_OP_READ           0x80000000u  /* [观察] 请求第 4 字：读 */
#define SG_MBX_OP_WRITE          0x80020000u  /* [观察] 请求第 4 字：写 */
#define SG_MBX_ACK_READ          0x8001u      /* [观察] 回复偏移 18 */
#define SG_MBX_ACK_WRITE         0x8003u

/* ===== A. BAR0：采集控制与块头 ====================================== */
#define SG_ACQ_ARM               0x201000u    /* 写 [未知] 启动前写 2；驱动只允许写 2 */
#define SG_ACQ_ARM_VALUE         2u
#define SG_HDR_ACK               0x202000u    /* 写 [观察] 读块头之前写 0xFFFFFFFF（照搬 Windows 顺序）*/
#define SG_HDR_ACK_VALUE         0xffffffffu
#define SG_HDR_READY             0x20206cu    /* 读 [观察] 非 0 = 一个数据块就绪 */
#define SG_ACQ_STATE             0x20209cu    /* 读 [观察] 等于 SG_ACQ_STATE_STOP 表示已停 */
#define SG_ACQ_STATE_STOP        0x53544f50u  /* [观察] ASCII "STOP" */

/* 块头字段（详见 docs/interfaces/I3_block_header.md） */
#define SG_HDR_WIDTH             0x202040u    /* [观察] 宽（像素），实测 4096 */
#define SG_HDR_LINES             0x202044u    /* [观察] 本块累计行数（最后一包时有效） */
#define SG_HDR_PIXFMT            0x202050u    /* [观察] 必须 = SG_PIXFMT_MONO8 */
#define SG_HDR_ZERO_A            0x202048u    /* [未知] 实测必须为 0 */
#define SG_HDR_ZERO_B            0x20204cu    /* [未知] 实测必须为 0 */
#define SG_HDR_SEQ               0x20201cu    /* [观察] bit30..0 包序号（从 1 起），bit31 = 本块最后一包 */
#define SG_HDR_PAYLOAD_LEN       0x202018u    /* [观察] 本包字节数 */
#define SG_HDR_DEST_OFFSET       0x202020u    /* [观察] 本包在整块里的偏移，必须等于已收字节数 */
#define SG_HDR_SRC_ADDR          0x202010u    /* [观察] 本包在卡上 DDR 的地址（DMA 源） */
#define SG_HDR_BLOCK_ID          0x202030u    /* [观察] 块编号，同一块内不变 */
#define SG_HDR_MAGIC             0x202098u    /* [观察] 必须 = SG_HDR_MAGIC_VALUE */
#define SG_HDR_MAX_LINES         0x202068u    /* [观察] 每块最大行数，实测 2000 */
#define SG_HDR_MAGIC_VALUE       0x53434255u  /* [观察] ASCII "SCBU" */
/* 读了但含义未知的块头寄存器 [未知]：
 * 0x202008 0x202014 0x202024 0x202028 0x20202c 0x202034 0x202038 0x20203c */

#define SG_PIXFMT_MONO8          0x01080001u  /* [观察] GenICam 风格的 Mono8 编码 */

/* ===== C. 逻辑地址（只能经邮箱访问）================================= */
#define SG_L_DDR_STATUS          0x10540024u  /* [推测] 0 未初始化 / 2 已校准；实测 2 */
#define SG_L_PIXEL_CLOCKS        0x10002020u  /* [推测] bit0 = Camera Link 时钟 1 有信号；实测 0x59，其余位未知 */
#define SG_L_CL_CONFIG           0x10601004u  /* [推测] 0 = Base；实测 0 */
#define SG_L_CAMERA_COLOR        0x10601008u  /* [推测] 0 = 黑白；实测 0 */
#define SG_L_CAMERA_DEPTH        0x1060100cu  /* [推测] 位深；实测 8 */
#define SG_L_ACQ_START           0x10601014u  /* [观察] 写 = 开始采集 */
#define SG_L_ACQ_STOP            0x10601018u  /* [观察] 写 = 停止采集 */
/* 待找：帧宽 / 每块行数 / tap 数的配置地址（目前卡固定 4096 宽，原因未知）*/

/* ===== B. BAR1：XDMA（只在 board/driver/ 里用）====================== */
#define SG_XDMA_C2H              0x1000u      /* [观察] C2H 通道 0 */
#define SG_XDMA_C2H_STATUS       (SG_XDMA_C2H + 0x40)
#define SG_XDMA_IRQ_BLOCK        0x2000u      /* [观察] ID 高 16 位 = 0x1fc2 */
#define SG_XDMA_CONFIG_BLOCK     0x3000u      /* [观察] ID 高 16 位 = 0x1fc3 */
#define SG_XDMA_SGDMA_DESC       0x5000u
#define SG_XDMA_DESC_CTRL_LAST   0xad4b0003u  /* [观察] 照搬 Windows 的单描述符控制字 */

#endif
