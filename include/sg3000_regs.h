/* SPDX-License-Identifier: MIT
 *
 * sg3000_regs.h —— SG3000 采集卡的所有地址和魔数，**只在这里定义一次**。
 *
 * 可信度标记（每一条都必须有）：
 *   [厂家]  来自厂家资料：采集卡自己存的 GenICam XML（docs/vendor/card_xml/，2026-11-14 实板读出）
 *   [观察]  来自观察 Windows 驱动 / VisionKit 的行为，并在 ZCU102 上实测通过
 *   [推测]  能用，但含义是推断出来的
 *   [推测+] 推测，且有旁证（如与厂家示例里的参数名对得上）
 *   [逆向]  来自反汇编 Windows 驱动 XDMA.sys / VisionKit DLL（2026-11-14，Gemini 做的，
 *           出处地址见 docs/vendor/reverse_engineering_2026-11-14.md）；还没在 Linux 上跑过
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
#define SG_MBX_WORD1_CARD        0u           /* [观察] 请求第 1 字：0 = 发给卡固件（下面串口邮箱是 0x10/0x12）*/

/* ===== A. BAR0：Camera Link 串口邮箱（和上面的卡邮箱同构）[逆向] ======
 * 端口 0 = 0x4110xx（Windows 节点 \\data_0/\\data_1），端口 1 = 0x4130xx（\\data_2/\\data_3）。
 * 报文 = 16 字节头（第 0 字 0x000155aa，第 1 字 SG_SER_WORD1，第 2 字 载荷长度|序号<<16，第 3 字 0）
 *        + 载荷，逐字写 TX_DATA，最后把"载荷长度+16"写 TX_LEN。
 * 卡邮箱 0x4120xx 的 +0x04/+0x08/+0x18/+0x28 也有同样的初始化（Windows 打开 \\grabber_0 时做），
 * 本仓库的卡邮箱代码没做也能用，所以不加。
 * 驱动白名单目前**不允许**访问串口这些地址（旧 .ko 会返回 -EINVAL），见 docs/interfaces/I7 第 4 节。 */
#define SG_SER_PORTS             2u
#define SG_SER_BASE(port)        (0x411000u + (uint32_t)(port) * 0x2000u)
#define SG_SER_CTRL(port)        (SG_SER_BASE(port) + 0x04) /* 读写 [逆向] = 0x04000000 表示已初始化 */
#define SG_SER_FLUSH_A(port)     (SG_SER_BASE(port) + 0x08) /* 写 [逆向] 初始化时写 0xa5，含义推测为清 FIFO */
#define SG_SER_TX_DATA(port)     (SG_SER_BASE(port) + 0x10) /* 写 [逆向] */
#define SG_SER_TX_LEN(port)      (SG_SER_BASE(port) + 0x14) /* 写 [逆向] 总字节数 = 载荷 + 16，写入即发送 */
#define SG_SER_FLUSH_B(port)     (SG_SER_BASE(port) + 0x18) /* 写 [逆向] 同 FLUSH_A */
#define SG_SER_RX_OCCUPANCY(port) (SG_SER_BASE(port) + 0x1c) /* 读 [逆向] 非 0 = 有回包 */
#define SG_SER_RX_DATA(port)     (SG_SER_BASE(port) + 0x20) /* 读 [逆向] */
#define SG_SER_RX_LEN(port)      (SG_SER_BASE(port) + 0x24) /* 读 [逆向] 回包总字节数（含 16 字节头）*/
#define SG_SER_FLUSH_C(port)     (SG_SER_BASE(port) + 0x28) /* 写 [逆向] 同 FLUSH_A */
#define SG_SER_WORD1(port)       (0x10u + 2u * (uint32_t)(port)) /* [逆向] 端口 0 = 0x10，端口 1 = 0x12 */
#define SG_SER_CTRL_READY        0x04000000u  /* [逆向] */
#define SG_SER_FLUSH_VALUE       0xa5u        /* [逆向] */

/* UART 分频寄存器：标准 16550 布局，偏移和 Xilinx AXI UART16550 IP 一致。[逆向]
 * 在 BAR0：XDMA.sys 用的是 bar[userBarIdx]，和 0x4110xx、0x4120xx 同一个指针（I7 第 3 节）。 */
#define SG_UART_BASE(port)       ((uint32_t)(port) << 16)
#define SG_UART_DLL(port)        (SG_UART_BASE(port) + 0x1000) /* 分频低 8 位（DLAB=1 时）*/
#define SG_UART_DLM(port)        (SG_UART_BASE(port) + 0x1004) /* 分频高 8 位（DLAB=1 时）*/
#define SG_UART_LCR(port)        (SG_UART_BASE(port) + 0x100c) /* bit7 = DLAB */
#define SG_UART_LCR_DLAB         0x80u
#define SG_UART_CLOCK_HZ         200000000u   /* 0xbebc200 */

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
#define SG_L_CAMERA_ENUM         0x10540010u  /* [逆向] 低字节 '1'(0x31) 时 VisionKit 认为卡有 2 路，否则 1 路。只读 */
#define SG_L_DDR_STATUS          0x10540024u  /* [厂家] DDRInitialStatus；0 未初始化 / 2 已校准；实测 2 */
#define SG_L_PIXEL_CLOCKS        0x10002020u  /* [厂家] bit0..2 = CameraLinkPixelClock1..3；实测 0x59 / 0x19，bit3+ XML 未定义 */
#define SG_L_CL_CONFIG           0x10601004u  /* [厂家] ClConfiguration：Base 0 / Medium 1 / Full 2 / EightyBit 4；实测 0 */
#define SG_L_CAMERA_COLOR        0x10601008u  /* [厂家] CameraColorType：Mono 0；实测 0 */
#define SG_L_CAMERA_DEPTH        0x1060100cu  /* [厂家] CameraPixelDepth：8/10/12/14/16；实测 8 */
#define SG_L_ACQ_START           0x10601014u  /* [厂家] AcquisitionStart，写 1 */
#define SG_L_ACQ_STOP            0x10601018u  /* [厂家] AcquisitionStop，写 1 */

/* 卡参数（[厂家]，全部 RW、小端 32 位；XML 里都受 TLParamsLocked 约束 → 只在停止采集时写）*/
#define SG_L_SCAN_TYPE           0x10601000u  /* CameraScanType：AreaScan 0 / LineScan 1 */
#define SG_L_TAP_GEOMETRY        0x10601010u  /* DeviceTapGeometry：Geometry_1X<n>_1Y = n（1..10）*/
#define SG_L_CAMERA_WIDTH        0x10203008u  /* CameraWidth：128..65535，8 位时步进 1 */
#define SG_L_CAMERA_HEIGHT       0x1020300cu  /* CameraHeight：每块行数，≥ 200 */
#define SG_L_X_OFFSET            0x10203014u  /* XOffset */
#define SG_L_Y_OFFSET            0x10203018u  /* YOffset */
#define SG_L_IMAGE_WIDTH         0x1020208cu  /* CameraImageWidth（分割图宽度）*/
#define SG_L_SPLIT_EN            0x10202070u  /* SplitEn：off 0 / on 1 */
#define SG_L_FRAME_LENGTH_METHOD 0x1020003cu  /* FrameLengthMethod：FixLength 0 / VariableLength 1 */
#define SG_L_OUT_WIDTH           0x10202040u  /* Width（RO）输出图宽 */
#define SG_L_OUT_HEIGHT          0x10202044u  /* Height（RO）输出图高 */
#define SG_L_DVAL_MODE           0x10002018u  /* DvalMode */
#define SG_L_FVAL_MODE           0x1000201cu  /* FvalMode */
#define SG_L_CL_CHIP_STATUS      0x10002038u  /* bit0..3 = CameraLinkChip/Chip1..3Status（RO）*/
#define SG_L_CL_FVAL_LVAL        0x10002028u  /* bit0..2 = Fval1..3，bit3..5 = Lval1..3（RO）*/
#define SG_L_LINE_TRIGGER_EN     0x10102040u  /* LineTriggerEnable / LineTriggerMode：0 / 1 */
#define SG_L_LINE_TRIGGER_SRC    0x10102044u  /* LineTriggerSource：Internal 0 / Software 1 / Line0.. 2.. */
#define SG_L_FRAME_TRIGGER_EN    0x10103040u  /* FrameTriggerEnable */
#define SG_L_FRAME_TRIGGER_SRC   0x10103044u  /* FrameTriggerSource */
#define SG_L_CL_CC1              0x1000402cu  /* CL_CC1：OFF 0 / ... */
#define SG_L_LINE_RX_COUNTER     0x10201034u  /* LineReceiveCounter（RO）*/
#define SG_L_FRAME_RX_COUNTER    0x1020102cu  /* FrameReceiveCounter（RO）*/
#define SG_L_LINE_PERIOD_10NS    0x105400b8u  /* （RO）行频 = 100 MHz / (值 + 1) */
#define SG_L_CAMERA_CLOCK_CNT    0x10540050u  /* （RO）相机时钟 = 819200 / (值 + 1) MHz（卡 XML 公式），8191..81919 之外无效 */
#define SG_L_FPGA_DATE           0x1054000cu  /* FPGADate（RO）*/
#define SG_L_VERSION_DATE        0x10540020u  /* VersionDate（RO）*/
/* 绝对不要写：0x10540018 DeviceReboot、0x10540040 ManualFPGAReload、0x10540044/4c FPGA 重载（XML 里有）*/
/* 待找：卡参数 CameraWidth / CameraHeight / DeviceTapGeometry 的地址。
 * 已确认 Windows 软件每次都设置 CameraWidth，Linux 从未设置 → 卡报告 4096 的直接原因。
 * [逆向] 地址都在卡自己的 XML 里，Linux 可以读出来：board/capture/sg_cardxml.h（URL 在逻辑地址 0x190）。
 * 拿到 XML 之前**不要试写**。 */

/* ===== B. BAR1：XDMA（只在 board/driver/ 里用）====================== */
#define SG_XDMA_C2H              0x1000u      /* [观察] C2H 通道 0 */
#define SG_XDMA_C2H_STATUS       (SG_XDMA_C2H + 0x40)
#define SG_XDMA_IRQ_BLOCK        0x2000u      /* [观察] ID 高 16 位 = 0x1fc2 */
#define SG_XDMA_CONFIG_BLOCK     0x3000u      /* [观察] ID 高 16 位 = 0x1fc3 */
#define SG_XDMA_SGDMA_DESC       0x5000u
#define SG_XDMA_DESC_CTRL_LAST   0xad4b0003u  /* [观察] 照搬 Windows 的单描述符控制字 */

#endif
