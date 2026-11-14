# 交接：现状和下一步（2026-11-14 晚，检测方案已补）

写给接手的 AI 助手（下一个会话、Fable、Gemini）和负责人。先读这一页，再按需读链接。

## 1. 现在到哪了（全部实板验证过，除非写了"待上板"）

| 环节 | 状态 | 依据 |
|---|---|---|
| 驱动 `sg3000_rx.ko`（新编，`cam_serial=1` 放开相机串口端口 0）| ✅ | 实测报告第 1–3 节 |
| 相机 GenCP：读型号、读 XML、读写寄存器（`camera-set` 白名单）| ✅ | 第 3、8 节；`docs/vendor/camera_xml/` |
| 相机和卡都切 **Base 3 Tap、16384 宽**（B 方案，一根线）| ✅ 采到 16384 × 2000 | 第 10 节 |
| 停采（读空 `SG_HDR_READY` FIFO 才转 STOP）| ✅ | 第 10.1 节 |
| 连续采集 `sg3000-stream` 10 kHz | ✅ 能跑，但**只收得动约一半** | 第 10.2、12 节 |
| DMA 提速 `dma_fast=1` | 🟡 Gemini 已用内核树编出 ARM64 `.ko`（`f818b3f`，带 `dma_fast` 参数，vermagic 5.15.19-xilinx-v2022.1），**待上板** | I4「DMA 吞吐」 |
| 测试图 / 调亮 / 15 kHz | 🟡 命令都写好了，**待上板** | BRINGUP 5⅞、5⁹ |
| 图偏暗、第 6/7 位从没出现 | ❓ 光不够还是丢位，测试图一拍就知道 | 第 11 节 |
| 检测方案 | 🟡 设计第 1 版已出：`docs/design/detection_plan.md`。待负责人回答 12 个问题、采购光源和样片 | 本文第 4 节 |
| 处理线程吞吐 | ⚠️ `sg_stats` / `sg_candidates` 在 A53 上估计只有 60–150 MB/s，**`dma_fast` 生效后会变成新瓶颈（队列满）** | 方案第 4.2 节 |

**最终目标**：15 kHz × 16384 像素 = 246 MB/s，单线 Base 3 Tap（读一行 64.3 µs < 一行 66.7 µs）。
速度依据见 `docs/decisions/D002_bandwidth.md`（视场 80 mm → 4.88 µm/像素，75 mm/s → 15.36 kHz）。

## 2. 下次进实验室（一条命令）

1. **`.ko` 已由 Gemini 编好并提交（`f818b3f`），这一步不用再做。** 以后驱动源码再改时照此重编：
   ```
   KDIR=<内核树> KERNEL_ANALYSIS=<kernel_analysis 目录> CROSS_COMPILE=aarch64-none-elf- sh board/driver/module.sh
   python tools/make_sd_bundle.py --bin-dir dist/glass-capture --ko board/driver/output/module/sg3000_rx.ko
   ```
   必须看到 `Compile and target ABI checks passed.`。提交 `dist/glass-capture/` 推到 `main`。ABI 检查不过就把报错发回来。
2. 用新 `dist/glass-capture/` 更新 SD 卡。只接第一根 Camera Link 线，不用网线。
3. 中控台：00 → 01 → 02 → **S 一键上板测试**（约 3–5 分钟）。
   - 板上等价：`sh /tmp/sg3000-board/board_session.sh`
   - 它依次做：驱动 `cam_serial=1` → 相机和卡切 Base 3 Tap / 16384 → 测试图 3 / 2 / 1（板上逐像素判渐变、查 8 个位）→ 实拍：默认、曝光 50 / 90 µs、模拟增益 4 → 30 秒连续采集（旧 DMA）→ 重载 `dma_fast=1`，测试图验数据 + 30 秒连续采集 → `LineRate 15000` + 30 秒连续采集。
   - 全部断电恢复：从不写相机 UserSetSave、卡 DeviceReboot / ManualFPGAReload。
4. 结果自动取回 `D:\SG3000_Captures\board-session\sg3000-session-*\`（`session.log`、`summary.txt`、缩略图）。**整个文件夹发给 Claude。**

看结果的要点：
- `Ramp ramp: x%256=MATCH`（或另外两个公式之一 MATCH）且 `missing=none` → 链路逐位正确。`missing=6,7` → 最高两位丢了，先查这个。
- `StreamSummary ... discontinuities=0`，`received` ≈ 秒数 × 行频 ÷ 2000，`wait_ms` 占大头 → 吞吐够。
- `dma_stats`：`wait_us` 大 = 等 PCIe；`copy_us` 大 = 拷贝慢（下一步做零拷贝）。
- **`ended` 行里 `error=-105`（ENOBUFS）且 `queue_full_events=1`**：不是 DMA 的问题，是处理线程（`--processor stats`）跟不上，队列满了。这种情况下 `received ÷ elapsed_ms` 仍然是 DMA 的真实吞吐下限。原因和修法见方案第 4.2 节、第 9 节第 1 项。
- `camera-set LineRate 15000` 被拒 → 日志里有相机允许的范围。D002 估算单线余量只有约 3.6%，相机最短行周期可能卡在 15 kHz 附近；被拒就用它给的上限，或者和导师确认能否降到约 14 kHz（70 mm/s）。

## 3. 协作规则（避免冲突）

- **只有 Claude / Fable 推 `main` 上的源码，并负责生成 `dist/` 里的程序。** Gemini 只提交 `dist/glass-capture/sg3000_rx.ko`（用 `make_sd_bundle.py --bin-dir dist/glass-capture --ko ...`，不重编程序），其余改动推 `gemini/*` 分支。
- 硬规则见 `GEMINI.md`（不猜寄存器、不放宽白名单、不改二进制、输出文字不改……）。
- 改完必须跑的测试也在 `GEMINI.md`。驱动改动还要用 Linux 内核头文件编一次（这里用的是 Ubuntu 的 6.8 头文件，只验语法和接口；真正的 ABI 检查要板子内核树）。

## 4. 检测方案（第 1 版已出）

**文档**：`docs/design/detection_plan.md`。先读它的第 0 节（一页结论）和第 9、10 节（下一步、要负责人回答的问题）。

要点：

- 30 µm = 6 像素，分辨率不是瓶颈；难点是**颗粒透明**、**景深只有 ±65 µm**、**光通量差一个数量级以上**（现在图暗多半是这个，不只是曝光）。
- 先搭明场透射，再用同一块真值样片试低角度暗场，按对比噪声比二选一（方案第 2.4 节）。
- 单线 Base 3 Tap 只能 Mono8，够用。
- 现有处理器在 A53 上跟不上 246 MB/s；换成"每列一对 8 位阈值 + NEON 快筛"后估计只占一个核的 20–40%，**不需要 PL**。
- 验收数字来自真值样片 + `verify/V4`；宣称"检出率 ≥ 99%"需要 ≥ 300 个真值颗粒。
- 最紧的是光源和样片的采购周期（路线图 M2）。

**配套工具（只在电脑上跑，不动板上代码）**：

```
python -I host/detect/study_detectability.py
```

重算方案里的表。`sim_particles.py` 生成"图 + 干净图 + 真值"，`detect_v1.py` 是下一版算法的原型（**不是** `sg_candidates` 的参考实现，那份合同不变），用法见方案附录 A。

**等负责人决定的事**：

1. 回答方案第 10 节的 12 个问题（验收口径、镜头、光源、玻璃、颗粒、运动方式……）。
2. 是否同意在下次上板前改板上代码（方案第 9 节第 1 项）：修处理线程瓶颈、加 `bench` 和噪声统计。不改也能上板，只是连续采集测试可能因队列满提前结束。

## 5. 其他待办（按优先级）

1. 上板后若 `dma_fast` 仍不够 246 MB/s：零拷贝（锁定用户页 + 分散聚集描述符，Xilinx 官方 XDMA 驱动的做法），按 `dma_stats` 哪段最慢再定。
2. `dma_fast` 验证通过后改成默认开（中控台 02 加载时带参数）。
3. 相机串口提速到 460800（相机 `DeviceSerialPortBaudRate` = SBRM `0x10004`，0x40；采集卡 UART 同步改分频）：读 XML / 寄存器快 4 倍，非必须。
4. A 方案（第二根线，EightyBit 10 Tap）：只有 15 kHz 被相机拒绝、又不能降速时才需要。
