# 2026-11-14 上板实测汇总（交接给 Claude）

基线提交：`00d0718`（`dist：从干净提交重新编译 ARM64 板上程序`）  
实板环境：Xilinx ZCU102 Rev1.0，`Linux 5.15.19-xilinx-v2022.1 aarch64`，PCIe `0000:01:00.0`（`0x4c54:0x3000`），通过 SD 卡加载并通过 USB 串口 `COM10`（`115200 8N1`）执行。

---

## 结论摘要

1. **新 `.ko` 已编译、通过目标内核 ABI 检查，并在实机上跑通全部现有采图链路**（`cam_serial=0` 默认行为与旧驱动完全一致）。
2. **`cam_serial=1` 串口白名单在实板上第一次就打通了相机 GenCP 通信**：在 `port=0, baud=115200` 读出真实相机身份：
   - `CameraManufacturer=JAI`
   - `CameraModel=SW-WS2-CL16K-50M-SDE`
   - **同时踩出一个单路卡硬件雷区**：`camera-probe --port 1` 会触发 `nwl-pcie fd0e0000.pcie: Slave error` / `synchronous external abort`（`sg_ioctl+0x184`），需整板重启恢复，建议按 `camera enum = 0x30424630`（单路卡）拦截 `port 1`。
3. **完成 BRINGUP 第 5½ 节 16K 宽度实验与掉电验证**：
   - 采集卡 `card-set CameraWidth 16384` 写入和读回均正常，确认**卡参数不掉电保存**（断电重启后恢复 `4096`）。
   - 只把卡侧 `CameraWidth` 设为 `16384`（保持 `ClConfiguration=0` Base、`DeviceTapGeometry=3` 不变）时，连续两次 `capture --timeout-ms 10000` 均在**约 2 秒内超时失败**（`AcquisitionStop cleanup failed: Connection timed out (-110)` / `Failed: Connection timed out (-110)`），但卡侧 `Width(out)=16384, Height(out)=2000`，且 `LineRxCounter` / `FrameRxCounter` 在持续计数。
4. **Windows 本地盘确认无 16K 相机 XML 缓存**：此前从未在 Windows 电脑插过 PCIe 采集卡，全盘仅存在 2022-11-10 VisionKit 安装包自带 XML。下一步获取 16K 相机 XML 只能走 **ZCU102 板上经 `cam_serial=1` GenCP 从相机读取**。
5. **基准图（`4096 × 2000`）位结构分析确认存在 Camera Link 像素位打包不匹配**：每 3 列周期出现 `0x47 / 0x46 / 0xc0` 掩码，24 bit 中仅 7 个位（`a0, a1, a6, b1, b2, c6, c7`）在动，其余 17 位全 0；拆出子位段后可见贯穿 2000 行的真实线阵纵向柱状纹理，幅度仅 `0..3`，且左右 4 等分无明显明暗跳变。

---

## 1. 新驱动编译与目标内核 ABI 验证（I7 第 8.1 节）

- 产出文件：`board/driver/output/module/sg3000_rx.ko`（`405,584` 字节，未进 git，按 `.gitignore` 规则保留本地产物）
- ABI 检查：`board/driver/output/module-abi-check.json` 全部通过
  - `vermagic`：`5.15.19-xilinx-v2022.1 SMP mod_unload aarch64`
  - `export_count`：`12382`
  - `missing_exports`：`[]`
  - `offline_abi_checks_passed`：`true`
- 打包与校验：连同 `00d0718` 下的 `sg3000-capture`、`sg3000-stream`、`sg_status` 一起打包至 SD 卡 `glass-capture/`，板上 `sha256sum -c` 四项全部 `OK`。

---

## 2. 默认参数加载新 `.ko` 验证（I7 第 8.2.1 节）

不带参数加载 `insmod /tmp/sg3000-board/sg3000_rx.ko`，完整跑通 `01 probe -> 02 prepare -> 02½ status -> 03 preflight -> 04 capture`：

- 驱动加载 dmesg：`sg3000_rx 0000:01:00.0: port0 C2H MM receiver ready at /dev/sg3000_rx_0000:01:00.0 (experimental)`
- `preflight`：`DDRInitialStatus=2, CameraLinkPixelClocks=0x00000019, CL=0, Color=0, Depth=8`
- `capture`：`Saved complete Mono8 frame: 4096 x 2000, 8192000 bytes, block 1, 1 packets`
- **结论**：不带 `cam_serial` 时，新 `.ko` 与旧驱动表现完全一致。

---

## 3. `cam_serial=1` 与 `camera-probe` 实测（I7 第 8.2.2–4 节）

重载驱动开启串口白名单：
```sh
rmmod sg3000_rx
insmod /tmp/sg3000-board/sg3000_rx.ko cam_serial=1
cat /sys/module/sg3000_rx/parameters/cam_serial   # -> Y
```

实测输出：

| 命令参数 | 结果 |
|---|---|
| `camera-probe --baud 115200`（默认 `port=0`） | ✅ **`CameraManufacturer=JAI`**，**`CameraModel=SW-WS2-CL16K-50M-SDE`** |
| `camera-probe --baud 9600` | `Failed: Connection timed out (-110)` |
| `camera-probe --baud 19200 / 38400 / 57600 / 460800` | `Failed: Protocol error (-71)` |
| `camera-probe --port 1 --baud 9600` | ❌ 触发 PCIe Slave Error 内核异常 |

### 单路卡 `port 1` 内核报错现场

```text
[  483.808278] nwl-pcie fd0e0000.pcie: Fatal Error Detected
[  483.808296] Internal error: synchronous external abort: 96000210 [#1] SMP
[  483.813598] nwl-pcie fd0e0000.pcie: Slave error
...
[  483.847269] CPU: 2 PID: 389 Comm: sg3000-capture Tainted: G           O      5.15.19-xilinx-v2022.1 #1
[  483.883032] pc : sg_ioctl+0x184/0x9b4 [sg3000_rx]
[  483.898744] lr : sg_ioctl+0x108/0x9b4 [sg3000_rx]
...
[  484.012724] x21: 00000000c0085331
```

当前卡 `camera enum = 0x30424630`（低字节 `0x30`，单通道卡），FPGA BAR0 中不存在第 2 路串口 UART/邮箱窗口；访问 `port 1` 偏移（`0x11000` / `0x413000`）直接产生 PCIe Master/Slave External Abort 并连续打印 AER 错误，需断电重启恢复。  
**建议**：在 `sg3000_rx.c`（或读取 `camera enum` 后在驱动/用户态双端）阻止单路卡访问 `port 1`。

---

## 4. BRINGUP 第 5½ 节：16K 宽度实验与掉电检查

### 4.1 改宽度前的 `== Card params ==` 完整基线

```text
== Driver ==
PCIe            0000:01:00.0   (vendor 0x4c54, device 0x3000)
ABI             1
engine_id       0x1fc10006
alignment       address 1, length 1, address_bits 64
max_transfer    65536 bytes
poisoned        0   (non-zero = driver has isolated the card)
== Preflight (read-only) ==
valid_mask      0x1f
DDR status      2   (0 = uninitialized, 2 = calibrated; both normal)
pixel clocks    0x00000019   bits set: 0 3 4   (bit0 = Camera Link clock 1; other bits unknown)
CL config       0   (0 = Base)
camera color    0   (0 = mono)
camera depth    8   (8 expected)
Preflight passed.
== Registers ==
SG_HDR_READY    0   (non-zero = a data block is waiting)
SG_ACQ_STATE    0x53544f50   ("STOP")
camera enum     0x30424630   (VisionKit: low byte 0x31 '1' = 2 channels, else 1)
== Card params (card XML, read-only) ==
CardParam CameraScanType     0x10601000 = 1 (0x1)   0 AreaScan / 1 LineScan
CardParam ClConfiguration    0x10601004 = 0 (0x0)   0 Base / 1 Medium / 2 Full / 4 EightyBit
CardParam DeviceTapGeometry  0x10601010 = 3 (0x3)   n = Geometry_1Xn_1Y
CardParam CameraPixelDepth   0x1060100c = 8 (0x8)   bits
CardParam CameraWidth        0x10203008 = 4096 (0x1000)   pixels per line
CardParam CameraHeight       0x1020300c = 2000 (0x7d0)   lines per block
CardParam XOffset            0x10203014 = 0 (0x0)
CardParam YOffset            0x10203018 = 0 (0x0)
CardParam Width(out)         0x10202040 = 4096 (0x1000)   read-only
CardParam Height(out)        0x10202044 = 0 (0x0)   read-only
CardParam CameraImageWidth   0x1020208c = 8192 (0x2000)   split image width
CardParam SplitEn            0x10202070 = 0 (0x0)
CardParam FrameLengthMethod  0x1020003c = 0 (0x0)   0 Fix / 1 Variable
CardParam DvalMode           0x10002018 = 0 (0x0)
CardParam FvalMode           0x1000201c = 0 (0x0)
CardParam CLChipStatus       0x10002038 = 3 (0x3)   bit0..3
CardParam CLFvalLval         0x10002028 = 9 (0x9)   bit0..2 Fval1..3, bit3..5 Lval1..3
CardParam LineTriggerEnable  0x10102040 = 0 (0x0)
CardParam LineTriggerSource  0x10102044 = 0 (0x0)   0 Internal / 1 Software / 2.. Line0..
CardParam FrameTriggerEnable 0x10103040 = 0 (0x0)
CardParam FrameTriggerSource 0x10103044 = 0 (0x0)
CardParam CL_CC1             0x1000402c = 0 (0x0)
CardParam LineRxCounter      0x10201034 = 0 (0x0)
CardParam FrameRxCounter     0x1020102c = 0 (0x0)
CardParam LinePeriod10ns     0x105400b8 = 9999 (0x270f)
CardParam CameraClockCnt     0x10540050 = 9636 (0x25a4)
CardParam FPGADate           0x1054000c = 809709872 (0x30433130)
CardParam VersionDate        0x10540020 = 20260629 (0x1352715)
CardParam LineRateHz          10000.0   (100 MHz / (LinePeriod10ns + 1); 0 = no lines)
CardParam CameraClockMHz      85.01   (819200 / (CameraClockCnt + 1), formula from card XML; 0 = out of range)
```

注意几个关键状态值：
- `CLChipStatus = 3`（bit0 和 bit1 均置位：2 颗 Camera Link RX 芯片已锁定时钟，不只 Base 1 颗）
- `CLFvalLval = 9`（`0b001001`：bit0 `Fval1=1`、bit3 `Lval1=1`）
- `LineRateHz = 10000.0`（10 kHz），`CameraClockMHz = 85.01`（~85 MHz）

### 4.2 对照采图 + 改 `CameraWidth = 16384` 后超时

1. 对照采集成功：`Saved complete Mono8 frame: 4096 x 2000, 8192000 bytes, block 1, 1 packets: /tmp/sg3000-console/frame-20261114-050310-76a29d.pgm`（已保存为 `frame-20261114-050310-baseline4k.pgm`）。
2. 执行写宽：`card-set CameraWidth 16384` -> `CardSet CameraWidth 0x10203008: 4096 -> 16384, readback 16384`。
3. 写入后首次 `sg_status`：`CameraWidth=16384`, `Width(out)=4096`, `Height(out)=2000`, `LineRxCounter=2958`, `FrameRxCounter=1`。
4. 保持其余参数不变，连续执行两次 `sg3000-capture capture --timeout-ms 10000`：
   - 第一次（`05:05:47` -> `05:05:49`）：约 2 秒失败：
     ```text
     DDRInitialStatus=2, CameraLinkPixelClocks=0x00000019, CL=0, Color=0, Depth=8
     AcquisitionStop cleanup failed: Connection timed out (-110)
     Failed: Connection timed out (-110)
     ```
     失败后 `sg_status`：`SG_HDR_READY=1`、`SG_ACQ_STATE=0x00000000`、`Width(out)=16384`、`Height(out)=2000`、`LineRxCounter=8770`、`FrameRxCounter=3`、`poisoned=0`；内核无新增报错。
   - 第二次（`05:07:39` -> `05:07:41`）：同样约 2 秒报相同错误，失败后 `sg_status`：`LineRxCounter=14676`、`FrameRxCounter=5`、`SG_HDR_READY=1`、`SG_ACQ_STATE=0x00000000`。
   - 等待约 60 秒后再次 `sg_status`：卡自动回到 `SG_HDR_READY=0`、`SG_ACQ_STATE=0x53544f50 ("STOP")`、`LineRxCounter=14676`、`FrameRxCounter=5`。

### 4.3 断电重启恢复默认检查

彻底断电重启后执行 `01 -> 02 -> 02½`：
- `CameraWidth = 4096 (0x1000)`、`Width(out) = 4096 (0x1000)`、`Height(out) = 0`、`LineRxCounter = 0`、`FrameRxCounter = 0`。
- **结论**：通过卡邮箱写入的 `CameraWidth` **不掉电保存**，断电后恢复出厂默认 `4096`。

---

## 5. 基准图（`frame-20261114-050310-baseline4k.pgm`）像素与位结构诊断

对 `4096 × 2000` PGM 原始字节做逐位统计（已存电脑本地 `D:\SG3000_Captures\frame-20261114-050310-baseline4k.pgm`，未入库）：

- **整体统计**：`min=0, max=192, mean=33.38, std=40.49`，百分位 `[0, 1, 5, 25, 50, 75, 95, 99, 100]` = `[0, 0, 1, 2, 4, 64, 128, 128, 192]`。
- **3 字节周期错位（与 10-30 旧图同构）**：
  - `col%3 == 0`：`OR mask = 0x47 (0100_0111)`，均值 `19.72`，常见值 `1, 65, 2, 66, 0, 64, ...`
  - `col%3 == 1`：`OR mask = 0x46 (0100_0110)`，均值 `2.55`，常见值 `2, 4, 0, 6`
  - `col%3 == 2`：`OR mask = 0xc0 (1100_0000)`，均值 `77.89`，常见值 `64, 128, 0, 192`
  - 对比 10 月 30 日 `frame-20261030-030603-9b2341.pgm`：`col%3` 掩码为 `0x43 / 0x06 / 0xc0`，完全是同一套 24-bit Camera Link 位排列，**24 bit 中仅有 7 位（`a0, a1, a6, b1, b2, c6, c7`）有效，其余 17 位全 0**。
- **子通道解包与相关性**：
  - `a0/a1` 相关系数 `-0.92`、`b1/b2` 为 `-0.95`、`c6/c7` 为 `-0.92`（主要是 `01` 和 `10` 即数值 `1`/`2` 在跳变，幅度只有 `0..3`），跨组相关系数 `<= 0.05`；
  - 按 `a & 3`、`(b >> 1) & 3`、`(c >> 6) & 3`、`(a >> 6) & 1` 解包渲染后，**4 个子通道都能看到贯穿全部 2000 行的清晰纵向线阵固定模式条纹**，证明是真实传感器行数据，而非总线噪声。
- **左右分布**：
  - 4 等分均值：`[0:1024]=36.23`、`[1024:2048]=32.43`、`[2048:3072]=32.19`、`[3072:4096]=32.67`；
  - 最左侧 1/4（`bins 0..1`）高位 `bit1 / bit6 / bit10 / bit23` 占比（`~0.30–0.38`）反而略高于右侧（`~0.21–0.27`），没有清晰的“左黑右亮”分界。

---

## 6. 建议 Claude 接下来推进的事项

1. **用已打通的 `cam_serial=1`（`port=0, baud=115200`）读出 16K 相机 GenCP XML 和当前相机寄存器配置**
   - 已有标准 GenCP 链路通向 `JAI SW-WS2-CL16K-50M-SDE`，可先按 GenCP 清单表（Manifest Table / XML 下载）拿到这台相机的真实 XML（无需再找 Windows 机器），核对 `cam_regs.h`，读出相机当前的 `Width`、`PixelFormat`、`TapGeometry`、`ClConfiguration`、`ExposureTime`、`LineRate`、`TestPattern` 等。
2. **定位并匹配 Camera Link 输出格式与 `CameraWidth=16384` 超时**
   - 实板 `CLChipStatus=3`（2 颗 CL 芯片锁定）与 `col%3` 掩码 `0x47 / 0x46 / 0xc0` 显示，相机实际输出与当前卡的 `ClConfiguration=0 (Base) / DeviceTapGeometry=3 / Depth=8` 不一致；另外只把卡侧改为 `CameraWidth=16384` 时为何 `AcquisitionStart/Stop` 邮箱或阻塞机制在 ~2 秒 `-110` 超时（而 `LinePeriod10ns` 下 10 kHz × 2000 行本来只需 0.2s，且 `FrameRxCounter` 仍涨到了 5、`SG_HDR_READY=1`），需要结合超时和 `sg_acq` / 卡配置顺序核查。
3. **补强单路卡 `port 1` 防护**
   - 在 `sg3000_rx.c` 和 `sg3000-capture camera-probe` 中依据 `camera enum` 拦截 `port >= 1` 的串口寄存器访问，避免再次触发 `nwl-pcie: Slave error` 导致整板需重启。
