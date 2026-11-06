# I4 · 驱动 ⇄ 用户程序

代码：`include/sg3000_rx_uapi.h`（25 行，原样迁移）。实现：`board/driver/sg3000_rx.c`。唯一调用者：`board/capture/sg_io_linux.c`。

设备文件：`/dev/sg3000_rx_<PCIe 地址>`，实测 `/dev/sg3000_rx_0000:01:00.0`。

| 命令 | 结构体 | 作用 |
|---|---|---|
| `SG_IOC_INFO` | `sg_info` | 读驱动信息：ABI、PCIe 位置、XDMA 引擎 ID、对齐要求、最大传输、**poisoned** |
| `SG_IOC_READ_REG` | `sg_reg` | 读 BAR0 一个寄存器（白名单内）|
| `SG_IOC_WRITE_REG` | `sg_reg` | 写 BAR0 一个寄存器（白名单内，且值受限）|
| `SG_IOC_DMA_READ` | `sg_dma` | 从卡上 DDR 的 `source` 搬 `length` 字节到用户缓冲区，≤ 64 KiB |

## 规则

- 成功返回 0，失败 `-1` 并设 errno（用户态拿到的是 `-errno`）
- DMA：驱动用自己的 `dma_alloc_coherent` 缓冲区收数，再拷给用户；**从不**把用户地址交给硬件
- DMA 等待方式：轮询写回计数，不用中断
- DMA 停不下来 → 驱动关闭总线主控、置 `poisoned = 1`，之后所有命令返回 -EIO，只能重新加载驱动
- 一次 64 KiB 的上限来自 `SG_DMA_CHUNK`；改大要同时改驱动缓冲区大小，并重新实测
