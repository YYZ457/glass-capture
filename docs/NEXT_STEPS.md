# 交接：现状和下一步（2026-11-14 晚）

写给接手的 AI 助手（下一个会话、Fable、Gemini）和负责人。先读这一页，再按需读链接。

## 1. 现在到哪了（全部实板验证过，除非写了"待上板"）

| 环节 | 状态 | 依据 |
|---|---|---|
| 驱动 `sg3000_rx.ko`（新编，`cam_serial=1` 放开相机串口端口 0）| ✅ | 实测报告第 1–3 节 |
| 相机 GenCP：读型号、读 XML、读写寄存器（`camera-set` 白名单）| ✅ | 第 3、8 节；`docs/vendor/camera_xml/` |
| 相机和卡都切 **Base 3 Tap、16384 宽**（B 方案，一根线）| ✅ 采到 16384 × 2000 | 第 10 节 |
| 停采（读空 `SG_HDR_READY` FIFO 才转 STOP）| ✅ | 第 10.1 节 |
| 连续采集 `sg3000-stream` 10 kHz | ✅ 能跑，但**只收得动约一半** | 第 10.2、12 节 |
| DMA 提速 `dma_fast=1` | 🟡 x86 头文件编过，**待 Gemini 编 ARM64 `.ko` 并上板** | I4「DMA 吞吐」 |
| 测试图 / 调亮 / 15 kHz | 🟡 命令都写好了，**待上板** | BRINGUP 5⅞、5⁹ |
| 图偏暗、第 6/7 位从没出现 | ❓ 光不够还是丢位，测试图一拍就知道 | 第 11 节 |
| 检测算法 | ⬜ 只有第一版骨架（`sg_candidates`），没有真值、没有设计 | 本文第 4 节 |

**最终目标**：15 kHz × 16384 像素 = 246 MB/s，单线 Base 3 Tap（读一行 64.3 µs < 一行 66.7 µs）。
速度依据见 `docs/decisions/D002_bandwidth.md`（视场 80 mm → 4.88 µm/像素，75 mm/s → 15.36 kHz）。

## 2. 下次进实验室（一条命令）

1. **Gemini 先在实验室外编好 `.ko`**（不需要硬件）：
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
- `camera-set LineRate 15000` 被拒 → 日志里有相机允许的范围。D002 估算单线余量只有约 3.6%，相机最短行周期可能卡在 15 kHz 附近；被拒就用它给的上限，或者和导师确认能否降到约 14 kHz（70 mm/s）。

## 3. 协作规则（避免冲突）

- **只有 Claude / Fable 推 `main` 上的源码，并负责生成 `dist/` 里的程序。** Gemini 只提交 `dist/glass-capture/sg3000_rx.ko`（用 `make_sd_bundle.py --bin-dir dist/glass-capture --ko ...`，不重编程序），其余改动推 `gemini/*` 分支。
- 硬规则见 `GEMINI.md`（不猜寄存器、不放宽白名单、不改二进制、输出文字不改……）。
- 改完必须跑的测试也在 `GEMINI.md`。驱动改动还要用 Linux 内核头文件编一次（这里用的是 Ubuntu 的 6.8 头文件，只验语法和接口；真正的 ABI 检查要板子内核树）。

## 4. 下一件大事：检测方案设计（不需要硬件，适合现在做）

**交付物**：`docs/design/detection_plan.md`（新文件；不改板上代码）。

**要回答的问题**：
1. **可检性**：30 µm 颗粒在 4.88 µm/像素下约 6 像素宽。这台相机是 3.5 µm 像元，视场 80 mm 意味着放大倍率约 0.72×。镜头 MTF、景深、玻璃厚度、正反面颗粒怎么区分？
2. **成像方式**：明场、暗场、同轴、低角度……哪种让 30 µm 颗粒对比度最高？要求的背景灰度（BRINGUP 5⅞ 建议 150–200）和动态范围。Mono8 够不够，要不要 10bit。
3. **算力预算**：246 MB/s 进入 4 × Cortex-A53（ZCU102，ZU9EG）。每个像素能花几个时钟？`sg_candidates`（背景校正 → 阈值 → 8 邻接连通域）在这个预算里够不够？需不需要 NEON、只处理感兴趣区域、隔行处理？什么条件下必须把预处理放进 PL（D001 规定了重新考虑的条件）？
4. **算法**：平场校正（每列增益 / 暗电流，需要白场和暗场标定）、阈值怎么定（固定 / 自适应 / 每列噪声倍数）、候选怎么分类（颗粒 / 划痕 / 灰尘 / 噪声），误检、漏检怎么评估。
5. **验证计划**：真值样本（标准颗粒或显微镜标定的玻璃片）怎么做、要多少；指标（检出率、误检率、尺寸误差）；`verify/V4_truth_match.py` 怎么用。
6. **风险和要负责人提供的信息**：列成清单。

**已知输入**：

| 项 | 值 | 来源 |
|---|---|---|
| 相机 | JAI SW-WS2-CL16K-50M-SDE（凌云光 SF6200），16384 × 2 像素，3.5 µm 像元，Mono8 / 10bit | 手册、相机 XML |
| 接口 | Camera Link Base 3 Tap，85 MHz（单线）| 实测 |
| 视场 / 像素 | 80 mm / 16384 ≈ 4.88 µm/像素 | D002 |
| 速度 / 行频 | 75 mm/s → 15.36 kHz；目标 15 kHz | D002、负责人 |
| 数据率 | 约 246 MB/s；PCIe 链路约 500 MB/s；千兆网约 110 MB/s（原图出不了板子，D002）| D002 |
| 处理 | ZCU102：4 × Cortex-A53 + ZU9EG FPGA；PL 目前不在数据通路（D001）| |
| 现有代码 | `board/processing/sg_candidates.c` ↔ `host/detect/candidates.py`（两份必须一致）；`verify/V1–V4` | |
| 实拍 | 明暗场景最大灰度只有 14（太暗，待调光）| 实测报告第 10 节 |

**还缺、要问负责人的**：镜头型号和光圈、工作距离、光源类型和角度、玻璃尺寸 / 厚度 / 是否镀膜、颗粒材质（玻璃屑 / 金属 / 有机物）、是否要区分正反面、验收时是否必须 75 mm/s、允许的漏检率和误检率。

## 5. 其他待办（按优先级）

1. 上板后若 `dma_fast` 仍不够 246 MB/s：零拷贝（锁定用户页 + 分散聚集描述符，Xilinx 官方 XDMA 驱动的做法），按 `dma_stats` 哪段最慢再定。
2. `dma_fast` 验证通过后改成默认开（中控台 02 加载时带参数）。
3. 相机串口提速到 460800（相机 `DeviceSerialPortBaudRate` = SBRM `0x10004`，0x40；采集卡 UART 同步改分频）：读 XML / 寄存器快 4 倍，非必须。
4. A 方案（第二根线，EightyBit 10 Tap）：只有 15 kHz 被相机拒绝、又不能降速时才需要。
