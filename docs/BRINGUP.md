# 上板步骤：从拷 SD 卡到采到第一帧

按顺序做。每一步都写了"应该看到什么"；看到的不一样就停下，把那一步中控台日志发给 Claude（见文末）。

## 0. 准备

| 东西 | 说明 |
|---|---|
| ZCU102 + SD 卡 | 现在能正常启动 Linux 的那张卡（里面有 `sg3000-linux-20261023/` 目录和驱动 `sg3000_rx.ko`） |
| USB 线 | 接 J83（串口，控制用） |
| 网线 | **板子网口直接连电脑网口**（可选，但强烈建议：回传一张图从约 7 分钟变成约 1 秒） |
| 相机 | 和以前一样接在 SG3000 卡的 Port 0，上电 |
| Windows 电脑 | 装 Python 3.10 以上 |

## 1. 拷程序到 SD 卡（每次程序更新都要做）

1. 板子断电，取出 SD 卡，插到电脑上。
2. 把仓库里的 **`dist/glass-capture` 整个文件夹**拷到 SD 卡第一个分区（FAT，Windows 能直接看到的那个）的根目录，
   和原来的 `sg3000-linux-20261023` 文件夹并列。原来的文件一个都不要删。
3. 弹出 SD 卡，插回板子。

`dist/glass-capture/` 里是本仓库编译好的 ARM64 程序（`sg3000-capture`、`sg3000-stream`、`sg_status`）和校验文件 `SHA256SUMS`。
驱动 `.ko` 继续用 SD 卡旧目录里那一份。

> 不要再用旧版中控台（`SG3000Workbench`）。它会自动往程序里打字节补丁（D003），新程序不需要。

## 2. 电脑上启动中控台（第一次）

在仓库根目录打开终端：

```
pip install -r host/console/requirements.txt
```

```
python host/console/launch.py
```

浏览器会打开中控台。图像默认存到 `D:\SG3000_Captures`（想换位置就先设环境变量 `SG_CAPTURES_DIR`）。

## 3. 配电脑网口（用网线时，第一次）

1. Windows 设置 → 网络 → 以太网（接板子的那个网卡）→ IP 分配 → 编辑 → 手动 → IPv4：
   - IP 地址 `192.168.1.10`，子网掩码 `255.255.255.0`，网关留空
2. 第一次在中控台点「配置网口并测试」时，Windows 防火墙会弹窗问 Python 能不能接收连接 → **勾选"专用"和"公用"，允许**。
   没弹窗但测试失败时：控制面板 → Windows Defender 防火墙 → 允许应用通过防火墙 → 找到 python.exe，两个都勾上。

## 4. 中控台里的操作顺序

板子上电，等 Linux 启动完（约 30 秒）。在「采集控制」页：

| 步骤 | 点什么 | 应该看到 |
|---|---|---|
| 1 | 选 J83 对应的串口 → 连接 → 00 唤醒终端 | 日志里出现 `#` 提示符，右上角"终端就绪" |
| 2 | 01 检查板卡 | "板卡身份检查通过" |
| 3 | 02 从 SD 加载程序 | `sha256sum` 三行 `OK`，"已从 SD 卡校验并加载新程序" |
| 4 | 02½ 只读体检 | 日志里 `== Driver ==` / `== Preflight ==` / `== Registers ==` 三段；`poisoned 0`；`Preflight passed.`；最后一行 `camera enum` 的值记下来（新加的，还不知道正常值） |
| 4½ | 02¾ 读取采集卡 XML | 1–2 分钟后"采集卡 XML 已存到电脑：…\card-xml\…"。失败时把日志里 `== Card XML ==` 那段发给 Claude |
| 5 | 03 检查采集条件 | `Preflight passed. Acquisition and DMA were not started.` |
| 6 | 填电脑 IP `192.168.1.10`、板子 IP `192.168.1.20` → 配置网口并测试 | "网口已通：板子 → 电脑 192.168.1.10:8770" |
| 7 | 04 采集一帧 | `Saved complete Mono8 frame: 4096 x 2000, 8192000 bytes ...` |
| 8 | 05 回传最新图像到电脑 | 几秒内"经网口"保存；下方图像查看器显示图 |

没有网线就跳过第 6 步，第 8 步会走串口（约 7 分钟，别中断）。

## 5. 连续采集 + 颗粒候选

「连续接收与处理」面板：处理后端选 **颗粒候选检测 · 第一版**，时长 30 秒 → 加载连续程序 → 开始连续接收。

- 每块的候选坐标（网口已配置时）存到 `D:\SG3000_Captures\candidates\*.json`
- 面板上"检测结论"显示每块候选个数
- 这是第一版算法，参数没对过真值，**候选不是缺陷判定**

## 5½. 16K 宽度实验（卡参数，旧驱动即可）

前提：第 4 节 1–8 步都通过，**网口已配置**（16K 一块约 32 MB，走串口要半小时以上）。

1. **先读，不写。** 点「02½ 只读体检」，把 `== Card params ==` 一整段记下来，尤其是这几项：
   - `CameraWidth`、`CameraHeight`、`DeviceTapGeometry`、`ClConfiguration`、`CameraScanType`
   - `LineRateHz`：相机实际行频；0 表示卡没收到行
   - `CameraClockMHz`：Camera Link 像素时钟
   - `CLFvalLval`
2. **对照组**：保持现在的宽度（多半是 4096），用黑纸遮住视野左半边，采一张并回传。
3. **改宽度**：在「采集卡参数」里选 `CameraWidth`，填 `16384`，点「写入采集卡」。
   - 成功会显示"4096 → 16384（已读回确认）"。
   - 板上命令行等价：`/tmp/sg3000-board/sg3000-capture card-set CameraWidth 16384`
4. **再采一帧**（遮挡保持不变）：应该看到 `Saved complete Mono8 frame: 16384 x 2000, 32768000 bytes`，然后回传。
5. **判断**：
   - 16K 图里左半边是黑的、右半边正常 → 相机本来就在输出 16384 宽，问题解决。
   - 图里出现 4 段重复，或有错位、斜纹 → 相机实际宽度或 Tap 和卡不一致。记下现象，**不要自己改 Tap**，发回来分析。
   - 04 超时（-110）或报错 → 改回 4096（`card-set CameraWidth 4096`，或者直接断电，卡参数断电即恢复默认），发日志回来。
6. **断电重启后**再做一次「02½」，看 `CameraWidth` 是不是回到了默认值。这能确认卡参数会不会保存。

规矩：
- 只改 `CameraWidth`，其他参数先不动。
- 只能在采集停止时写（程序会自己检查）。
- ~~`ClConfiguration` 不要改：相机现在是 Base。~~ 2026-11-14 实测推翻了这个假设（`CLChipStatus=3`、位错乱），见第 5¾ 节。

**2026-11-14 结果**：写入读回正常、断电恢复 4096；但只改宽度后采图 2 秒报 -110。原因待第 5¾ 节第 5 步的 `FailedStage` 定位。

## 5¾. 相机 XML 与 Camera Link 模式对齐（需要新驱动 cam_serial=1）

**为什么要做**：4096 宽的基准图里 24 位只有 7 位在动（`col%3` 掩码 `0x47/0x46/0xc0`）：相机和卡的 Camera Link 模式不一致。
（当时把 `CLChipStatus=3` 读成"两颗芯片锁钟"是错的：按卡 XML，bit0 是"总状态"、bit1 才是芯片 1，所以 3 = 只有芯片 1 在用且正常。）
先从相机本身读出它的 XML 和实际配置，再让卡跟相机一致。

前提：SD 卡用本次的新 `dist/glass-capture/`，加新编译的 `sg3000_rx.ko`（第 5 步之前不重编也能跑，端口 1 由程序拦截）。
**不要用 `--port 1`**（单路卡，会让 PCIe 总线报错死机）。

1. 「02 从 SD 加载程序」→「02½ 只读体检」。
2. **C1 相机信息**：应看到 `CameraModel=SW-WS2-CL16K-50M-SDE` 和 `CameraManifest[0] ... type=zip ...`。中控台会自动以 `cam_serial=1` 重载驱动。
   - 板上等价：`/tmp/sg3000-board/sg3000-capture camera-probe`
3. **C2 读取相机 XML**：几十 KB 约 10–60 秒。成功显示"相机 XML 已存到电脑 …，SHA1 与相机一致"。文件在 `D:\SG3000_Captures\camera-xml\`。
   - 板上等价：`/tmp/sg3000-board/sg3000-capture camera-xml /tmp/sg3000-board/camera-xml`
4. **C3 读相机寄存器**：会列出 `Width`、`PixelFormat`、`ClConfiguration`、`DeviceTapGeometry`、`AcquisitionLineRate`、`ExposureTime` 等的当前值，存成 `camera-regs-*.txt`。
   **把 XML 文件和这个 txt 一起发给 Claude**，然后再做下面的 A 或 B。

**2026-11-14 结果**（实测报告第 8 节）：第 1–4 步全部通过。相机确认是 **16384 × Mono8 × EightyBit（10 Tap）× 85 MHz × 10 kHz**。
卡切到 EightyBit 后预检报 -95（程序写死了 Base，已改），而且 **第 2、3 路像素时钟始终为 0**（`0x10002020` bit1、bit2；卡 XML 定义）→ **第二根 Camera Link 线上没有信号**。

接下来二选一。**A 是最终方案**（50 kHz 要靠它），**B 不用第二根线、今天就能出 16K 图**（10 kHz 以内够用）。

### A 方案：接第二根线，卡切 EightyBit

1. **先查线**（断电操作）：相机背后两个 Camera Link 口、采集卡上两个口，**两根线都要接**，且相机口 1 ↔ 卡口 1、相机口 2 ↔ 卡口 2，不要交叉。记下卡上口的丝印。
2. 上电，「02」→「02½」。此时卡还是 Base，看 `pixel clocks`：**bit1、bit2 应该变成 1**（例如 `0x5f` 或 `0x1f`）。还是 0 就停下，拍照发回来（线、口、丝印）。
3. `card-set ClConfiguration 4`（卡固件会自动把 Tap 设为 10）→ `card-set CameraWidth 16384`。
4. 「02½」：应 `Preflight passed`，`CLChipStatus = 15` 或至少 bit0 = 1，`LineRateHz ≈ 10000`。如果看到 `missing clocks 1 2 (EightyBit needs BOTH Camera Link cables...)`，就是第二根线仍没信号。
5. 采一帧：`sg3000-capture capture --output /tmp/sg3000-console/deca16k.pgm --timeout-ms 10000 --control-timeout-ms 2000`，经网口回传看图。

### B 方案：相机和卡都切 Base 3 Tap（只用一根线）

依据：相机 XML 里 Base 时可选 `Geometry_1X2 / 1X3`。Base 3 Tap × 85 MHz 一行 16384 像素约 64 µs，当前 10 kHz 行频（100 µs/行）装得下；上限约 15 kHz。

1. 断电重启（卡回到 Base / 3 Tap / 4096，相机回到 EightyBit）。「02」→「02½」。
2. **改相机**（中控台「写入相机」，或板上命令），顺序固定：
   1. `sg3000-capture camera-set ClConfiguration 0` → 应显示 `3 -> 0, readback 0`
   2. `sg3000-capture camera-set DeviceTapGeometry 3` → `readback 3`（相机 XML 规定只有 Base 时才能改 Tap）
3. 「C3 读相机寄存器」确认 `ClConfiguration = 0 (Base)`、`DeviceTapGeometry = 3`。
4. **卡保持 Base / 3 Tap**，只改宽度：`card-set CameraWidth 16384`。
5. 「02½」：`Preflight passed`，`LineRateHz ≈ 10000`。
6. 采一帧（同 A 第 5 步）。判断：
   - 正常的话，**24 位里不再只有 7 位在动**，灰度分布连续。遮住一半视野应该能看到对应的半边变暗。
   - 如果每行末尾有 1–2 像素错位（16384 不能被 3 整除），换 `camera-set DeviceTapGeometry 2` 并 `card-set DeviceTapGeometry 2` 再试（2 Tap 一行 96 µs，10 kHz 刚好装得下）。
7. 恢复：相机和卡都是断电恢复（`camera-set` 不写 User Set，不会改变相机的上电默认）。

### 两个方案共用

- 失败时把 `FailedStage` / `StopFailedStage` / `LastHeader` 三行和「02½」全文原样发回来。
- 只用这些写操作：卡 `card-set`（5 个参数）、相机 `camera-set`（2 个参数）。其余全部只读。

## 6. 采到图之后可以马上做的验证（电脑上，不用板子）

```
python -I verify/V3_dark_stats.py D:\SG3000_Captures\某张盖镜头盖的图.pgm --save-background bg.csv
```

```
python -I verify/V2_line_integrity.py D:\SG3000_Captures\某张图.pgm
```

```
python -I host/detect/candidates.py D:\SG3000_Captures\某张图.pgm --threshold 25 > cands.json
```

- V3：盖上镜头盖采 2~3 张，看暗噪声；`bg.csv` 就是检测用的背景标定
- V2：用黑纸遮住传感器（视野）左半边采一张，回答"宽度到底是 4096 还是 16384"

## 7. 出问题时

| 现象 | 多半是 | 先做 |
|---|---|---|
| 02 报"没有 glass-capture 文件夹" | 第 1 步没拷，或拷进了子文件夹 | 检查 SD 根目录下直接就是 `glass-capture\sg3000-capture` |
| 02 报 `sha256sum` FAILED | 拷贝不完整 | 重新拷整个文件夹 |
| 02 报 `can't open 'sg3000-capture\r'` | 旧版中控台 + Windows 检出把 SHA256SUMS 变成了 CRLF | 用新版中控台（已自动去掉 `\r`）；或者删掉本地 `dist` 文件夹后 `git checkout -- dist` 重新检出 |
| 02½ 报"打不开采集卡设备" | 驱动没加载 / 别的程序占着卡 | 点「读取内核日志」，看 `sg3000_rx` 相关行 |
| 03 报 `Link has been severed (-67)` | Camera Link 时钟没锁定 | 相机上电了吗？线插在 Port 0 吗？看体检里 `pixel clocks` 那一行 |
| 03 报 `(-95)` | CL 配置 / 彩色 / 位深不是 Base / 黑白 / 8 位 | 相机配置被改过；用 VisionKit 恢复 |
| 04 超时 `(-110)` | 相机没在出图（触发模式？） | 记下日志，不要连续重试 |
| 网口测试失败 | 网线 / 电脑 IP / 防火墙 | 按第 3 步逐项检查 |

## 8. 需要发给 Claude 的东西

每次上板后，把下面这些发过来，下一步就能继续推进：

1. 中控台「导出本次日志」的文件（整个流程的完整记录）
2. **02½ 只读体检**的完整输出（驱动信息 + 时钟寄存器每一位）
3. 采到的第一张图（PGM），以及遮住左半边的那张（V2 用）
4. 盖镜头盖采的 2~3 张图（V3 用）
5. 如果有：Windows 上 VisionKit 导出的**这台 16K 相机**的 GenICam XML（`cam_regs.h` 现在用的是 8K 型号的地址）
5½. 16K 宽度实验：改宽度前后两次 `== Card params ==` 全段，以及两张遮半边的图
5¾. **C2 读到的相机 XML 文件 + C3 的 `camera-regs-*.txt`**；第 5 步失败时的 `FailedStage` / `LastHeader` 几行
6. **「02¾ 读取采集卡 XML」读到的文件**（`D:\SG3000_Captures\card-xml\` 里；`CameraWidth` 的地址在里面，见 I2 第 5 节）
