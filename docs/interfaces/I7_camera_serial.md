# I7 · 相机控制（GenCP over Camera Link 串口）

可信度标记同 I2。本文新增的 **[逆向]** = 反汇编 Windows 驱动和 DLL 得来，出处见 `docs/vendor/reverse_engineering_2026-11-14.md`，还没在 Linux 上跑过。

## 1. 整条链路

```
cam_config（宽度、行频、曝光……）  地址表 cam_regs.h [参考8K]
   │ cam_gencp_read32 / write32          ⬜ 缺 GenCP 串口帧的校验算法
GenCP 二进制帧
   │ cam_serial_send / recv              🟡 已写，假卡测过
SG3000 串口邮箱 0x4110xx / 0x4130xx [逆向]
   │ 驱动 sg3000_rx.ko                  ⬜ 白名单还没放开
采集卡 FPGA 里的 UART16550 → Camera Link 串口线 → 相机
```

## 2. 串口邮箱（和卡邮箱 0x4120xx 同构）[逆向]

| | 端口 0 | 端口 1 |
|---|---|---|
| 基址 | `0x411000` | `0x413000` |
| Windows 节点 | `\data_0` / `\data_1` | `\data_2` / `\data_3` |
| 请求第 1 字 | `0x10` | `0x12` |

| 偏移 | 方向 | 作用 |
|---|---|---|
| +0x04 | 写 | 设完波特率后写 `0x04000000`，含义未知 |
| +0x10 | 写 | 发送数据，逐字写 |
| +0x14 | 写 | 发送总字节数 = 载荷 + 16，写入即发出 |
| +0x1c | 读 | 非 0 = 有回包 |
| +0x20 | 读 | 回包数据，逐字读 |
| +0x24 | 读 | 回包总字节数（含 16 字节头） |

**发送报文**：16 字节头，后面接载荷，全部小端 32 位字。
- 第 0 字 `0x000155aa`
- 第 1 字 `0x10` 或 `0x12`
- 第 2 字 `载荷长度 | 序号<<16`
- 第 3 字 `0`
- 载荷最后一个字不足 4 字节时补 0。补 0 是 [推测]，按卡邮箱的做法。

**回包**：
1. 先读 4 字头。
2. 头第 0 字低 16 位是 `0x55aa` 时，载荷长度取头偏移 8 处的 u16；否则取"总字节数 − 16"。

代码在 `board/camera_ctrl/cam_serial.c`：`cam_serial_pack`、`cam_serial_payload_len`、`cam_serial_send`、`cam_serial_recv`。单元测试在 `board/tests/unit_test.c`。

## 3. 波特率 [逆向]

- 只接受 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600。
- 分频值 = (200 MHz + 8·baud) / (16·baud)，由 `cam_uart_divisor()` 计算。
- **921600 误差 −3.1%，不要用。** 相机支持的最高 460800，误差 +0.47%。

写入顺序。下面的"UART 基址"= 端口号 << 16：
1. LCR（基址 + 0x100c）置 bit7，即 DLAB = 1。
2. DLM（基址 + 0x1004）写分频值高 8 位。
3. DLL（基址 + 0x1000）写分频值低 8 位。
4. LCR 清 bit7。
5. 串口邮箱 +0x04 写 `0x04000000`。

偏移和 Xilinx AXI UART16550 IP 的寄存器表完全一致，所以卡上 FPGA 用的就是这个标准 IP。

⚠️ **卡点：这几个地址在哪个 BAR，还没确认。** BAR1 的 0x1000 是 XDMA C2H 引擎，往那里写会弄乱 DMA。按推断，AXI 总线上的 IP 应该在 BAR0，但必须先在 `XDMA.sys` `0x140003150` 处核对 `bar` 指针的来源。`cam_serial_set_baud()` 在确认之前恒返回 `-ENOSYS`。

相机上电默认波特率多半是 9600，这是 Camera Link 惯例，待确认。如果卡上 UART 的默认分频也是 9600，不设波特率可能就能通信。这也要实测。

## 4. 驱动白名单要放开什么（上线前提）

板上现在用的旧 `.ko` 只认卡邮箱。`cam_serial_send/recv` 在旧驱动上会得到 `-EINVAL`，不会碰硬件。要让串口在 Linux 上工作，`board/driver/sg3000_rx.c` 的 `sg_reg_allowed()` 需要**只加**下面这些，并重新编译 `.ko`（`board/driver/module.sh`，需要板子内核源码树）：

| 方向 | 地址（端口 0；端口 1 加 0x2000） | 限制 |
|---|---|---|
| 写 | 0x411010 | — |
| 写 | 0x411014 | 值 17..16+512 |
| 写 | 0x411004 | 值只能是 0x04000000（设波特率才需要） |
| 读 | 0x41101c、0x411020、0x411024 | — |
| 读写 | BAR0 0x1000、0x1004、0x100c（端口 1：0x11000…） | **BAR 确认后才加**；LCR 只允许改 bit7 |

## 5. GenCP 帧（载荷里装什么）

- VisionKit 走标准 EMVA GenCP 二进制协议，preamble `0x0100`。
- **还缺**：串口版 GenCP 帧前缀里两个 CRC16 的算法和字节序。两种来源：
  - EMVA 官网免费下载的 GenCP 标准文档；
  - 让 Gemini 在 `VisionKitApi.dll` 里找组 GenCP 包、算校验的函数。
- 有了它就能写 `cam_gencp_read32/write32`：ReadMem / WriteMem 命令。cam_config 已经写好，压在这两个函数上面。
- 相机寄存器地址：`board/camera_ctrl/cam_regs.h`，全部 [参考8K]，要用 16K 相机自己的 XML 核对。

## 6. 已证伪

| 说法 | 结论 |
|---|---|
| 波特率分频"250 或 50" | ❌ 那是 `virtualserial.dll` 里的毫秒超时 |
| 用 JAI ASCII 短命令（`WN`、`LR`、`SBDRT`……）配置相机 | ❌ VisionKit 全部二进制 0 命中。相机固件可能仍留着 ASCII 口，可在 Windows 串口终端发 `ID?` 试，但不是第一路线 |

## 7. 短期替代（不写代码）

在 Windows 上用 VisionKit 设好相机：宽度 16384、行频、曝光、`ClConfiguration`、Tap、像素时钟。然后存进 User Set，并设为上电默认（`UserSetSave` / `UserSetDefault`）。
相机之后每次上电自动用这套参数，Linux 端暂时不需要串口。

## 8. 待办（按顺序）

- [ ] Windows：核对 `XDMA.sys` `0x140003150` 的 `bar` 是不是 BAR0
- [ ] 拿到 GenCP 串口帧的 CRC 算法
- [ ] 改驱动白名单（第 4 节），重新编译 `.ko`，上板跑 `cam_serial`
- [ ] 16K 相机的 XML，核对 `cam_regs.h`
