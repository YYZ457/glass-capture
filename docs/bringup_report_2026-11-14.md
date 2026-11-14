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

---

## 7. Claude 复核与已做的改动（同日，提交见 git log）

### 7.1 16K 宽度 2 秒 -110：现在能定位，但根因很可能是 CL 模式不对

- 2 秒 = 两个 1 秒超时相加：主流程一个 1 秒（**开始命令邮箱回包** 或 **首包 DMA**，两者默认都是 1000 ms），加上清理时 **停止命令邮箱** 又一个 1 秒。帧超时是 10 秒，所以不是"等块头"超时。
- `poisoned 0` 排除不了 DMA：引擎根本没启动就超时时，驱动不会隔离。所以光看现有日志分不出是哪一个。
- 已加诊断：`capture` 失败时多打印 `FailedStage`（开始 / 等块头 / 读块头 / 查块头 / DMA / 停止各一个名字）、`StopFailedStage`、`LastHeader`（块号、宽、行、包长、卡上地址）。另加 `--control-timeout-ms 1..2000`，用来验证是不是开始命令回包慢。
- 但更根本的：卡按 Base 3 Tap 解码，而相机证据指向 Deca。先对齐模式（BRINGUP 第 5¾ 节），再看宽度。

### 7.2 为什么判断相机在 Deca（待相机寄存器确认）

| 证据 | 说明 |
|---|---|
| `CLChipStatus = 3` | 两颗 CL 接收芯片都锁了钟。Base 只用一颗；第二颗有钟说明相机在第二根线上也在发 |
| 16384 × 50 kHz | = 819 Mpx/s。Base 3 Tap × 85 MHz 只有 255 Mpx/s；Deca 10 Tap × 85 MHz = 850 Mpx/s，刚好够 |
| 手册 | "CameraLink Deca & Base"，数据位宽 Mono8 / 10bit |
| 位图 | 24 位里只有 7 位在动、按 `col%3` 周期出现：80 位模式的位分配和 Base 端口 A/B/C 不同，按 Base 解就会是"错位的几位" |
| 卡支持 | 卡 XML：`ClConfiguration` 有 `EightyBit = 4`，`Geometry_1X10_1Y` 只在 EightyBit + 8 位时可用（`N2110`）|

### 7.3 相机 XML 不需要 Windows

VisionKit 读"设备 XML"走的就是 GenCP 标准清单表（BRM `0x1D0` → 条目 → XML 地址/大小），已从 `VisionKitApi.dll` 确认地址和格式（I7 第 9 节）。新增：
- 板上：`camera-probe`（补全 BRM 和清单表）、`camera-xml`、`camera-regs`、`camera-read`，全部只读。
- 中控台：C1 相机信息 / C2 读取相机 XML（自动核对 SHA1）/ C3 读相机寄存器（`host/console/genicam.py` 解析 XML 并翻译枚举；已用采集卡 XML 验证：`CameraWidth → 0x10203008` 等全部对上）。
- 相机命令会自动检查 `cam_serial`：`N` 时以 `cam_serial=1` 重载驱动；旧驱动直接报错。

### 7.4 端口 1 防护

- 用户态：`--port 1` 先读 camera enum，单路卡直接拒绝，不碰任何端口 1 寄存器（立即生效）。
- 驱动：白名单删掉端口 1 的 `0x4130xx` 和 `0x110xx`（`driver_whitelist_test` 覆盖）。**需要重编 `.ko`**（I7 第 8.1 节同样的命令）。


---

## 8. `12c1249` 上板实测（BRINGUP 第 5¾ 节：相机 XML / 寄存器 / EightyBit 预检与 CL 芯片状态）

- **测试条件**：拉取 `origin/main` `12c1249`（源码 `891003c` + 重编译 ARM64 工具），Windows 上用隔离镜像脚本重编通过 ABI 校验的 `sg3000_rx.ko`（白名单已移除端口 1，405,288 bytes），四份文件全部通过 `SHA256SUMS` 校验后上板实测。

### 8.1 C1 / C2：相机真实 XML 与 GenCP 清单（只读）

- **C1 (`camera-probe`)**：中控台自动将驱动重载为 `cam_serial=Y`，`port=0 baud=115200` 读出：
  - `CameraManufacturer=JAI`
  - `CameraModel=SW-WS2-CL16K-50M-SDE`
  - `CameraDeviceVersion=300.001.22`
  - `CameraSerialNumber=SF1AJFF12602`
  - `CameraGenCPVersion=1.2`
  - `CameraCapability=0x00000e0100000000 MaxResponseMs=2000`
  - `CameraSBRMAddress=0x10000`
  - `CameraManifestTable=0x100000 entries=1`
  - `CameraManifest[0] file=0.0.3 type=zip addr=0x800000 size=18279 sha1=none`
- **C2 (`camera-xml`)**：用时 `2 s` 下载完成（18,279 bytes），已归档到仓库 [docs/vendor/camera_xml/SW-WS2-CL16K-50M-SDE_0.0.3.zip](vendor/camera_xml/SW-WS2-CL16K-50M-SDE_0.0.3.zip)，解压内部文件为 [SF6000V300R001C22XB003.xml](vendor/camera_xml/SF6000V300R001C22XB003.xml)（178,934 bytes）。

### 8.2 C3 + 补充相机寄存器实测（全部只读，实锤相机输出模式）

1. **C3 默认清单（`21 ok, 0 failed`，完整记录见 [camera-regs-20261114-055406.txt](vendor/camera_xml/camera-regs-20261114-055406.txt)）**：
   - `DeviceScanType (0x01600004) = 1`（`Linescan`）
   - `WidthMax (0x01303038) = 16384`，`Width (0x01303040) = 16384`，`Height (0x01303044) = 1`，`OffsetX (0x01303048) = 0`
   - `PixelFormat (0x01401040) = 17301505 (0x1080001)`（`Mono8`）
   - **`ClConfiguration (0x01501008) = 3`（`EightyBit`）**
   - **`DeviceTapGeometry (0x01501010) = 10`（`Geometry_1X10`）**
   - `ExposureMode (0x01001080) = 1`（`Timed`），`DigitalGain (0x0120604c) = 256`（`1.0`），`BlackLevel (0x0120c050) = 0`
   - `BinningHorizontal (0x01302040) = 1`（`1x1`），`ReverseX (0x01301000) = 0`（`False`），`TestPattern (0x01100000) = 0`（`Off`）
   - `UserSetSelector (0x01600310) = 1`（`UserSet1`），`UserSetDefaultSelector (0x01600320) = 1`（`UserSet1`）
2. **针对相机真实 XML 命名补充读取 9 项时钟/行频/曝光寄存器（`sg3000-capture camera-regs`，`9 ok, 0 failed`）**：
   > 注：这台 JAI 相机 XML 使用了自定义节点名（`CameraLinkClkFrequency`、`InternalLineRate`、`MeasuredLineRate`、`AllExposureTime`、`LineTriggerMode`），而非 SFNC 默认名，且换算引用常数 `SensorClockFrequency = 96`（MHz），因此 `genicam.py` 默认名表会跳过这几项。
   - `CameraLinkClkFrequency (0x01501060) = 85`（`85M`，与卡测 `CameraClockMHz = 85.00~85.01` 完全一致）
   - `LineTriggerMode (0x01001004) = 0`（`Off`，内触发连续行频），`LineTriggerSource (0x01006044) = 1`（`CC1`）
   - `AllExposureTime (0x01001010) = 4800` / `MeasuredExposureTime (0x01001088) = 4800` → `4800 / 96 = 50.0 us`
   - `InternalLineRate (0x0100101c) = 9600` / `MeasuredLineRate (0x01001038) = 9600` → `96,000,000 / 9600 = 10000.0 Hz`（与卡测 `LineRateHz = 10000.0` 完全一致）
   - `FieldCount (0x01208000) = 1`，`CameratvalidCount (0x01501064) = 27427`

**结论**：彻底证实了 Claude 第 7 节判断——相机侧确实出的是 **16384 宽 × Mono8 × EightyBit（`Geometry_1X10`）× 85 MHz × 10 kHz 行频**。

### 8.3 第 5 步实测：卡切 `ClConfiguration=4 / CameraWidth=16384` 被预检拦下，且发现 CL 芯片状态变化

1. **写入卡参数读回确认**：
   - `card-set ClConfiguration 4`：`0 → 4`（已读回确认）。紧接着执行 `card-set DeviceTapGeometry 10` 时返回 `10 → 10`，说明卡固件在 `ClConfiguration=4` 时已自动把 `DeviceTapGeometry` 切到了 `10`。
   - `card-set CameraWidth 16384`：`4096 → 16384`（已读回确认）。
2. **`02½ status` 与 `04 capture` 均在 `preflight` 立即失败（`-95`）**：
   - `capture` 原文输出：
     ```text
     SG3000 0000:01:00.0, C2H0 AXI MM, ABI 1
     DDRInitialStatus=2, CameraLinkPixelClocks=0x00000019, CL=4, Color=0, Depth=8
     FailedStage: preflight
     LastHeader none (no block header was read)
     Failed: Operation not supported (-95)
     ```
   - 根因：`board/capture/sg_preflight.c:77` 仍写死 `if (p->cl_configuration != 0 || p->camera_color != 0 || p->camera_depth != 8u) return -ENOTSUP;`，尚未放开 `cl_configuration == 4`（`EightyBit`）。
3. **关键附加现象：`ClConfiguration` 切为 `4` 后卡侧 FPGA 状态变化**：

| 寄存器 / 指标 | `ClConfiguration=0`（改前，宽 4096） | `ClConfiguration=4`（宽 16384，Tap=10） | 切回 `ClConfiguration=0`（宽仍 16384） |
| :--- | :--- | :--- | :--- |
| `CLChipStatus (0x10002038)` | `3` (`bit0=1, bit1=1`) | **`2` (`bit0=0` Link Failed, `bit1=1` Link Ok)** | 立即恢复 `3` |
| `pixel clocks (0x10002020)` | `0x59` (`bits 0, 3, 4, 6`) | `0x19` (`bits 0, 3, 4`) | 保持 `0x19` |
| `LinePeriod10ns (0x105400b8)` | `9999` → **`LineRateHz = 10000.0`** | **`377634666 (0x16823f6a)` → `0.3 Hz`** | 立即恢复 `9999` → **`10000.0`** |
| `CameraClockMHz (0x10540050)` | `85.01` | `85.00` | `85.01` |
| `CLFvalLval (0x10002028)` | `9` | `9` | `9` |

### 8.4 给 Claude 的下一步交接问题

1. **放开软件预检**：`board/capture/sg_preflight.c:77`（及 `sg_status.c` 提示）需要允许 `ClConfiguration = 4`（EightyBit）。
2. **排查切 `ClConfiguration = 4` 后 `CLChipStatus` 掉 bit0、卡测行频停在 `0.3 Hz` 的原因**：
   - 卡 XML 定义 `0x10002038` bit0..3 对应 `CameraLinkChipStatus / Chip1Status / Chip2Status / Chip3Status`，同时在卡 XML 中紧挨着还有 `CameraLinkReLink (0x10002C00, WO, cmd=1)` 和 `CameraLinkAutoLink (0x10002034, RW)`，以及分图相关 `Width(out)=4096`、`CameraImageWidth=8192`、`SplitEn=0`。
   - 需要确认 `LusterGraber.dll` / VisionKit 在切换 `ClConfiguration=4 (EightyBit)` 时的完整初始化顺序（是否需要 `CameraLinkReLink` / 软复位，或者其它寄存器联动），否则即便放开 `preflight` 的 `-95`，卡 FPGA 在 `ClConfiguration=4` 下未锁进 `Chip0` 且 `LinePeriod10ns` 不计数时，开始采集仍可能收不到行数据。
3. **`host/console/genicam.py` 选配优化**：`DEFAULT_FEATURES` 可补充 JAI 命名（`CameraLinkClkFrequency`、`InternalLineRate`、`MeasuredLineRate`、`AllExposureTime`、`LineTriggerMode`）以便 C3 直接读出时钟、行频与曝光。
