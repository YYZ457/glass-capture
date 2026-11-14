# I4 · 驱动 ⇄ 用户程序

代码：`include/sg3000_rx_uapi.h`（25 行，原样迁移）。实现：`board/driver/sg3000_rx.c`。唯一调用者：`board/capture/sg_io_linux.c`。

设备文件：`/dev/sg3000_rx_<PCIe 地址>`，实测 `/dev/sg3000_rx_0000:01:00.0`。

| 命令 | 结构体 | 作用 |
|---|---|---|
| `SG_IOC_INFO` | `sg_info` | 读驱动信息：ABI、PCIe 位置、XDMA 引擎 ID、对齐要求、最大传输、**poisoned** |
| `SG_IOC_READ_REG` | `sg_reg` | 读 BAR0 一个寄存器（白名单内）|
| `SG_IOC_WRITE_REG` | `sg_reg` | 写 BAR0 一个寄存器（白名单内，且值受限）|
| `SG_IOC_DMA_READ` | `sg_dma` | 从卡上 DDR 的 `source` 搬 `length` 字节到用户缓冲区，≤ `sg_info.max_transfer`（默认 64 KiB，`dma_fast=1` 时 1 MiB）|

## 规则

- 成功返回 0，失败 `-1` 并设 errno（用户态拿到的是 `-errno`）
- DMA：驱动用自己的 `dma_alloc_coherent` 缓冲区收数，再拷给用户；**从不**把用户地址交给硬件
- DMA 等待方式：轮询写回计数，不用中断
- DMA 停不下来 → 驱动关闭总线主控、置 `poisoned = 1`，之后所有命令返回 -EIO，只能重新加载驱动
- 每次上限看 `sg_info.max_transfer`：默认 64 KiB（`SG_DMA_CHUNK`，所有驱动都支持），`dma_fast=1` 时 1 MiB（`SG_DMA_MAX_CHUNK`）。用户态按它自动选，不改 ABI 版本

## DMA 吞吐（2026-11-14 起）

**为什么要改**：最终目标 15 kHz × 16384 = **246 MB/s**；2 秒连续采集实测只收走相机产出的一半（约 50–80 MB/s，停采时卡里压着 5 块）。

**旧路径每 64 KiB 做的事**：`memset` 清零 → 启动 DMA → `usleep_range(50,100)` 轮询 → `copy_to_user`。缓冲区是 `dma_alloc_coherent`；ZynqMP 的 PCIe 若不做缓存一致，这块内存不走缓存，清零和拷贝都很慢。一块 32 MB 要 500 次。

**`dma_fast=1`（默认关，不加参数时行为和旧驱动完全一样）**：
- 缓冲区改成普通内存（`alloc_pages`，开机时清零）+ `dma_map_page` 一次映射；每次传输前后用 `dma_sync_single_for_device / for_cpu` 维护缓存，拷贝走缓存。
- 每次最多 1 MiB（`sg_info.max_transfer`），32 MB 一块只要 32 次。
- 不再每次 `memset`（缓冲区开机清零，之后只存放本驱动 DMA 来的数据）。
- 轮询间隔 10–20 µs。
- 分配不到 2 MiB 连续内存时自动退回旧路径，`dmesg` 有 `dma_fast: ...` 警告。

**计时**：`/sys/bus/pci/devices/0000:01:00.0/dma_stats`（两种模式都有）
```
mode=fast max_transfer=1048576 transfers=.. bytes=.. prep_us=.. wait_us=.. copy_us=.. errors=..
```
- `prep_us`：开始前（清零或刷缓存 + 写描述符）；`wait_us`：等 DMA 完成；`copy_us`：拷给用户程序。
- 往文件里写任意内容即清零：`echo 0 > .../dma_stats`。
- 用户态另有：`capture` 成功后打印 `DMAStats chunk=.. transfers=.. bytes=.. dma_ms=.. MBps=.. wait_ms=..`；`sg3000-stream` 的 `ended` 里多 `dma_chunk`、`dma_bytes`、`dma_ms`、`wait_ms`。
- **怎么读**：`wait_ms`（等卡出数据的时间）接近 0、`dma_ms` 占满 = DMA 跟不上相机；`wait_ms` 占大头 = 板子在等相机，吞吐够了。

**新增的内核符号**（ABI 检查会逐个核对）：`alloc_pages`、`__free_pages`、`dma_map_page_attrs`、`dma_unmap_page_attrs`、`dma_sync_single_for_cpu`、`dma_sync_single_for_device`、`device_create_file`、`device_remove_file`、`sysfs_emit`、`_dev_warn`。
