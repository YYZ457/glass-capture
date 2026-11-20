# 采集协议走读：一帧数据怎么从采集卡到 ARM 内存

*2026-11-20T14:23:35Z by Showboat 0.6.1*
<!-- showboat-id: 7b128683-eec1-432e-903d-573d5adf1ffe -->

这是用 [Showboat](https://simonwillison.net/2026/Feb/10/showboat-and-rodney/) 生成的可执行文档，所有输出都是真实执行时捕获的。复验（仓库根目录）：

    uvx showboat verify docs/demos/capture_protocol.md --workdir board

**这份文档回答一个问题：板上程序采一帧图，到底对采集卡做了什么？** 读 `board/capture/` 的代码很难直接看出来，因为逻辑分散在 5 个文件里（会话、邮箱、预检、块、开始/停止）。所以这里换个办法：在假卡外面包一层日志，把 `sg_capture_once()` 采一帧时**每一次寄存器读写和 DMA** 按顺序打印出来，看真实的调用顺序。

跟踪程序是 `docs/demos/src/trace_one_frame.c`。它把 `board/tests/fake_card_test.c` 原样 `#include` 进来复用那张假卡，只在回调表（`struct sg_capture_io`）外面包一层打印。这正是项目的核心设计：`board/capture/` 只通过这张回调表碰硬件，板上接 ioctl（`sg_io_linux.c`），电脑上接假卡。

## 0. 先认识两种地址

`include/sg3000_regs.h` 把地址分成三类，跟踪里会同时出现其中两类：

- **A 类：BAR0 寄存器**（如 `SG_HDR_READY`、`SG_ACQ_ARM`）。CPU 经驱动的 ioctl 直接读写，一次一个 32 位字。
- **C 类：逻辑地址**（`SG_L_` 开头，如 `SG_L_ACQ_START`）。CPU **不能直接碰**，要往 A 类的"邮箱"寄存器里逐字写一个请求报文，由卡上固件代为读写，再从邮箱读回复。
- （B 类是 BAR1 上的 XDMA 引擎，只有内核驱动碰，跟踪里看到的"DMA"就是驱动替我们操作它。）

## 1. 一次邮箱事务长什么样

先用 `--raw` 看第一次邮箱读（读 `SG_L_DDR_STATUS`，卡上 DDR 是否初始化好）的原始寄存器操作：

```bash
sh ../docs/demos/src/build_trace.sh && out/trace_one_frame --raw | sed -n 1,9p
```

```output
  1  R SG_MBX_RX_OCCUPANCY 
  2  R SG_MBX_TX_ROOM       = 0x00000008
  3  W SG_MBX_TX_DATA       （逐字写请求报文）
       （上一行连续 7 次）
  4  W SG_MBX_TX_LEN = 28   ── 邮箱请求：读 SG_L_DDR_STATUS
  5  R SG_MBX_RX_OCCUPANCY 
  6  R SG_MBX_RX_LEN        = 0x00000020
  7  R SG_MBX_RX_DATA      
       （上一行连续 8 次）
```

一次"读一个逻辑地址"= 先确认回复窗口是空的、发送窗口有空间 → 逐字写 7 个字的请求 → 写长度 28 字节触发发送 → 等回复 → 读回复长度 → 逐字读 8 个字的回复。回复里带一个**递增的 ACK 序号**，代码会检查它是否连续（不连续就报 `-EPROTO`，见假卡场景 2）。

这一套很啰嗦，所以下面的跟踪默认把每次邮箱事务压成一行。

## 2. 完整的单帧采集

假卡的块很小（16 像素宽 × 8 行 = 128 字节），分 2 包、每包 64 字节，块号从 7 开始。

```bash
out/trace_one_frame
```

```output
  1  邮箱  读 SG_L_DDR_STATUS    → 2   （回复 ACK #1）
  2  邮箱  读 SG_L_PIXEL_CLOCKS  → 89   （回复 ACK #2）
  3  邮箱  读 SG_L_CL_CONFIG     → 0   （回复 ACK #3）
  4  邮箱  读 SG_L_CAMERA_COLOR  → 0   （回复 ACK #4）
  5  邮箱  读 SG_L_CAMERA_DEPTH  → 8   （回复 ACK #5）
  6  R SG_HDR_READY         = 0x00000000
  7  W SG_ACQ_ARM           = 0x00000002
  8  邮箱  写 SG_L_ACQ_START     = 1   （回复 ACK #6）
  9  R SG_HDR_READY         = 0x00000001
 10  W SG_HDR_ACK           = 0xffffffff
 11  R SG_HDR_SRC_ADDR      = 0x00001000
 12  R 0x202014(无名)     = 0x00000000
 13  R SG_HDR_BLOCK_ID      = 0x00000007
 14  R 0x202034(无名)     = 0x00000000
 15  R SG_HDR_WIDTH         = 0x00000010
 16  R SG_HDR_ZERO_A        = 0x00000000
 17  R SG_HDR_ZERO_B        = 0x00000000
 18  R 0x202038(无名)     = 0x00000000
 19  R 0x20203c(无名)     = 0x00000000
 20  R SG_HDR_LINES         = 0x00000008
 21  R SG_HDR_PIXFMT        = 0x01080001
 22  R 0x202008(无名)     = 0x00000000
 23  R SG_HDR_SEQ           = 0x00000001
 24  R SG_HDR_PAYLOAD_LEN   = 0x00000040
 25  R SG_HDR_DEST_OFFSET   = 0x00000000
 26  R 0x202024(无名)     = 0x00000000
 27  R 0x202028(无名)     = 0x00000000
 28  R 0x20202c(无名)     = 0x00000000
 29  R SG_HDR_MAX_LINES     = 0x00000008
 30  R SG_HDR_MAGIC         = 0x53434255
 31  DMA 卡上 DDR 0x1000 → 内存，64 字节
 32  R SG_HDR_READY         = 0x00000001
 33  W SG_HDR_ACK           = 0xffffffff
 34  R SG_HDR_SRC_ADDR      = 0x00001040
 35  R 0x202014(无名)     = 0x00000000
 36  R SG_HDR_BLOCK_ID      = 0x00000007
 37  R 0x202034(无名)     = 0x00000000
 38  R SG_HDR_WIDTH         = 0x00000010
 39  R SG_HDR_ZERO_A        = 0x00000000
 40  R SG_HDR_ZERO_B        = 0x00000000
 41  R 0x202038(无名)     = 0x00000000
 42  R 0x20203c(无名)     = 0x00000000
 43  R SG_HDR_LINES         = 0x00000008
 44  R SG_HDR_PIXFMT        = 0x01080001
 45  R 0x202008(无名)     = 0x00000000
 46  R SG_HDR_SEQ           = 0x80000002
 47  R SG_HDR_PAYLOAD_LEN   = 0x00000040
 48  R SG_HDR_DEST_OFFSET   = 0x00000040
 49  R 0x202024(无名)     = 0x00000000
 50  R 0x202028(无名)     = 0x00000000
 51  R 0x20202c(无名)     = 0x00000000
 52  R SG_HDR_MAX_LINES     = 0x00000008
 53  R SG_HDR_MAGIC         = 0x53434255
 54  DMA 卡上 DDR 0x1040 → 内存，64 字节
 55  邮箱  写 SG_L_ACQ_STOP      = 1   （回复 ACK #7）
 56  R SG_ACQ_STATE         = 0x53544f50

sg_capture_once = 0，帧 16x8，块号 7，stop_error = 0
```

按阶段拆开读：

| 步骤 | 阶段 | 在做什么 | 代码 |
|---|---|---|---|
| 1–5 | 预检 | 经邮箱只读 5 个逻辑地址：DDR 已校准（=2）、CameraLink 像素时钟（89 = 0x59）、链路配置（0 = Base）、单色、8 位 | `sg_preflight.c` |
| 6 | 开始前检查 | `SG_HDR_READY` 必须为 0；如果开始前就已经有块就绪，说明上一次没停干净，报 `-EBUSY` 且不发 START（假卡场景 7） | `sg_acq.c` |
| 7–8 | 开始 | 先写 `SG_ACQ_ARM = 2`（含义不明，照搬 Windows），再经邮箱写 `SG_L_ACQ_START = 1` | `sg_acq.c` |
| 9–31 | 第 1 包 | 等 `SG_HDR_READY` 非 0 → 写 `SG_HDR_ACK = 0xffffffff` → **按固定顺序读 20 个包头寄存器** → 检查 → DMA 搬 64 字节 | `sg_block.c` |
| 32–54 | 第 2 包 | 同上；注意 `SG_HDR_SEQ = 0x80000002`：最高位是"本块最后一包"，低位是包序号 2；`DEST_OFFSET = 0x40` 正好接在第 1 包之后 | `sg_block.c` |
| 55–56 | 停止 | 经邮箱写 `SG_L_ACQ_STOP = 1`，轮询 `SG_ACQ_STATE` 直到等于 `0x53544f50`（ASCII "STOP"） | `sg_acq.c` |

几个读代码时容易忽略的点：

- **包头读 20 个寄存器，其中 8 个"无名"。** 它们的含义没人知道，只是照着 Windows 软件的顺序原样读一遍，存进 `opaque[]`。为什么不省掉？因为不知道读取本身有没有副作用（`SG_HDR_READY` 就是"读即弹出"的，见 `board_tests.md`），照搬是最安全的选择。
- **一个块 = 若干包，一个包 = 若干次 DMA。** 每次 DMA 最多 64 KiB（驱动 `dma_fast=1` 时 1 MiB）。实板上 4096×2000 的块是 1 个包、8,192,000 字节，所以按 64 KiB 要 DMA 125 次；这里假卡的包只有 64 字节，所以每包 1 次。
- **魔数是 ASCII。** `SG_HDR_MAGIC = 0x53434255` 是 "SCBU"，`SG_ACQ_STATE` 停止时是 "STOP"。看到这种值时往 ASCII 想。

## 3. 失败时：包头魔数错

`--bad-magic` 让假卡给出错误的块头魔数（假卡场景 6），看出错后程序做什么：

```bash
out/trace_one_frame --bad-magic | sed -n "6,8p;29,40p"
```

```output
  6  R SG_HDR_READY         = 0x00000000
  7  W SG_ACQ_ARM           = 0x00000002
  8  邮箱  写 SG_L_ACQ_START     = 1   （回复 ACK #6）
 29  R SG_HDR_MAX_LINES     = 0x00000008
 30  R SG_HDR_MAGIC         = 0x00000000
 31  邮箱  写 SG_L_ACQ_STOP      = 1   （回复 ACK #7）
 32  R SG_ACQ_STATE         = 0x53544f50

sg_capture_once = -71，帧 0x0，块号 0，stop_error = 0
primary_stage = "check block header"，start_attempted = 1，stop_attempted = 1，帧内存 = 已释放
```

第 1 个包头读完、检查失败（`-71` 即 `-EPROTO`）后，**没有 DMA**，但**仍然发了 STOP 并等到卡真正停下**。这是 `sg_acq.h` 开头写明的规则：

> begin 只要尝试过 START（哪怕失败），end 就一定会做有超时的 STOP

原因很实际：卡一旦开始采集就会不停往 DDR 里写块，程序如果出错就直接退出，卡会一直处于采集状态，下一次启动时在"开始前检查"那一步就会撞上 `-EBUSY`。结果里同时保存 `primary_error`（第一个错误）和 `stop_error`（停止本身有没有出错），两者分开报告。

## 4. 这些寄存器知识有多可靠

`sg3000_regs.h` 给每个地址都标了可信度，这是理解整个项目的关键背景：**采集卡没有 Linux 驱动和公开手册，寄存器知识主要来自观察和逆向 Windows 软件。**

```bash
grep -oE '\[(厂家|观察|推测\+?|逆向|未知)\]' ../include/sg3000_regs.h | sort | uniq -c | sort -rn
```

```output
     34 [观察]
     17 [逆向]
     11 [厂家]
      5 [未知]
      3 [推测]
      1 [推测+]
```

```bash
sed -n '/可信度标记/,/\[未知\]/p' ../include/sg3000_regs.h
```

```output
 * 可信度标记（每一条都必须有）：
 *   [厂家]  来自厂家资料：采集卡自己存的 GenICam XML（docs/vendor/card_xml/，2026-11-14 实板读出）
 *   [观察]  来自观察 Windows 驱动 / VisionKit 的行为，并在 ZCU102 上实测通过
 *   [推测]  能用，但含义是推断出来的
 *   [推测+] 推测，且有旁证（如与厂家示例里的参数名对得上）
 *   [逆向]  来自反汇编 Windows 驱动 XDMA.sys / VisionKit DLL（2026-11-14，Gemini 做的，
 *           出处地址见 docs/vendor/reverse_engineering_2026-11-14.md）；还没在 Linux 上跑过
 *   [未知]  只知道要读/写它，不知道它是什么
```

跟踪里出现的寄存器大多是 [观察]（在 ZCU102 上实测通过），但 `SG_ACQ_ARM`（写 2）是 [未知]，`SG_MBX_TX_ROOM` 和 `SG_MBX_RX_OCCUPANCY` 是 [推测]。[逆向] 的 17 条来自反汇编 Windows 驱动，还没在 Linux 上跑过。

所以改 `board/capture/` 时的原则是：**能照搬 Windows 顺序就照搬，不要"优化"掉看起来多余的读写**，除非实板验证过。

## 5. 小结：一帧的生命

    预检（邮箱只读）→ HDR_READY 必须为 0 → ARM=2 → 邮箱 START
      └─ 循环每个包：等 HDR_READY → 写 ACK → 读 20 个包头寄存器 → 校验 → 分块 DMA
          直到 SEQ 最高位 = 1（最后一包）
    → 邮箱 STOP → 轮询 ACQ_STATE == "STOP"（期间读 HDR_READY 排空 FIFO）
    出错的话：跳过剩下的步骤，但 STOP 照发

连续采集（`sg3000-stream`）用的是同一套 `sg_stream_begin / next / end`，只是中间那个循环一直跑。注意 `sg_acq.h` 里的状态：单帧已实测，**连续 begin/next/end 的这套接口从没在板上跑过**（不过 ARCHITECTURE.md 记录 `sg3000-stream --source hardware` 已实机通过——两份文档的状态不一致，值得核对）。

