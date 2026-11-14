# I7 · 相机控制（GenCP over Camera Link 串口）

可信度标记同 I2。**[逆向]** 表示照 Windows 驱动和 DLL 的反汇编写的，出处地址见 `docs/vendor/reverse_engineering_2026-11-14.md` 第 2 部分。这类内容还没在 Linux 上跑过。

## 1. 整条链路和状态

```
cam_config（宽度、行频、曝光……）      地址表 cam_regs.h [16K确认]         🟡 地址已按相机 XML 核对，写路径未上板
   │ cam_xml：清单表 → 读相机 XML        GenCP 标准引导 [逆向+标准]           ✅ 实板 2 s 读出 18,279 B zip
   │ cam_gencp_read32 / write32           GenCP 包 + 反码校验和 [逆向]          ✅ 实板读出 JAI / 型号
   │ cam_serial_send / recv               串口邮箱 0x4110xx [逆向]              ✅ 实板
   │ cam_serial_open / set_baud           初始化 + UART16550 分频（BAR0）[逆向]  ✅ 实板，115200
   │ 驱动 sg3000_rx.ko cam_serial=1       白名单只放开端口 0                     ✅ 实板（端口 1 已去掉，需重编 .ko）
采集卡 FPGA 里的 UART16550 → Camera Link 串口线 → 相机
```

板上命令（全部只读，只发 GenCP ReadMem；都要 `insmod sg3000_rx.ko cam_serial=1`；默认 `--port 0 --baud 115200`）：

| 命令 | 作用 | 中控台按钮 |
|---|---|---|
| `sg3000-capture camera-probe` | 厂商、型号、版本、序列号、GenCP 版本、XML 清单表 | C1 相机信息 |
| `sg3000-capture camera-xml DIR` | 按清单表从相机读出它自己的 GenICam XML（.zip/.xml），打印 SHA1 | C2 读取相机 XML |
| `sg3000-capture camera-regs NAME@ADDR:LEN ...` | 按清单读一串寄存器 | C3 读相机寄存器 |
| `sg3000-capture camera-read ADDR [LEN]` | 读任意地址，十六进制打印 | — |
| `sg3000-capture camera-set ClConfiguration\|DeviceTapGeometry V` | **唯一的相机写**：CL 模式 0 Base / 3 EightyBit；Tap 2/3（仅 Base）。核对型号、写后读回，不存 User Set | 写入相机 |

相机自己的 XML：`docs/vendor/camera_xml/SW-WS2-CL16K-50M-SDE_0.0.3.zip`（解压为 `SF6000V300R001C22XB003.xml`）。注意这台相机的 `ClConfiguration` 枚举 EightyBit = **3**，采集卡的是 **4**，两边编号不同。

C3 的清单由电脑上的 `host/console/genicam.py` 从相机 XML 生成，读回的值也由它翻译成名字和枚举（`python host/console/genicam.py 相机.zip --decode 板上输出.txt`）。

**端口 1**：2026-11-14 实板上单路卡（camera enum `0x30424630`）跑 `--port 1` 触发 `nwl-pcie Slave error`，整板要重启。现在两道防线：
1. 用户态：`--port 1` 先读 camera enum，低字节不是 `'1'`（双路）就拒绝，不碰任何端口 1 寄存器。
2. 驱动：白名单删掉了端口 1（`0x4130xx`、`0x110xx`）。**这一条要重编 `.ko` 才生效**；重编前靠第 1 条。

## 2. 串口邮箱 [逆向]

| | 端口 0 | 端口 1 |
|---|---|---|
| 基址（BAR0）| `0x411000` | `0x413000` |
| Windows 节点 | `\data_0` / `\data_1` | `\data_2` / `\data_3` |
| 请求第 1 字 | `0x10` | `0x12` |

| 偏移 | 方向 | 作用 |
|---|---|---|
| +0x04 | 读写 | = `0x04000000` 表示已初始化 |
| +0x08、+0x18、+0x28 | 写 | 初始化时写 `0xa5`（推测是清 FIFO）|
| +0x10 | 写 | 发送数据，逐字写 |
| +0x14 | 写 | 发送总字节数 = 载荷 + 16，写入即发出 |
| +0x1c | 读 | 非 0 = 有回包 |
| +0x20 | 读 | 回包数据，逐字读 |
| +0x24 | 读 | 回包总字节数（含 16 字节头）|

**打开**（`cam_serial_open`）：
1. 读 +0x04，已经是 `0x04000000` 就直接返回。
2. 否则最多 3 轮：往 +0x28、+0x18、+0x08 各写 `0xa5`，再读 +0x1c，读到 0 就停。
3. 最后写 +0x04 = `0x04000000`。

**发送**：
- 包头 4 个字：`0x000155aa`、`0x10` 或 `0x12`、`载荷长度 | 序号<<16`（序号第一包是 0）、`0`。
- 接着写载荷，小端 32 位字，不足补 0。
- 最后写 TX_LEN。

**接收**：
1. 读 4 个字的头。头第 0 字低 16 位是 `0x55aa` 时，载荷长度取头偏移 8 处的 u16；否则取"总长度 − 16"。
2. 再读 ceil(载荷长度 / 4) 个字。

## 3. 波特率 [逆向]

UART 是标准 16550，即 Xilinx AXI UART16550 IP，位置在 **BAR0 的 `(端口 << 16) + 0x1000`**：
- DLL：+0x1000
- DLM：+0x1004
- LCR：+0x100c

**为什么确定是 BAR0**：Windows 驱动写分频寄存器和写 0x4110xx 用的是同一个指针 `bar[userBarIdx]`，而 0x4110xx 只能落在 16 MiB 的 BAR0。

写入顺序（`cam_serial_set_baud`，逐条照 XDMA.sys）：
1. 读 LCR。
2. LCR 置 bit7（DLAB = 1）。
3. DLL 写 `0xff`。原因未知，照抄。
4. DLM 写分频值高 8 位。
5. DLL 写分频值低 8 位。
6. LCR 写回原值。
7. 读串口邮箱 +0x04，bit26 没置位就写 `0x04000000`。

- 分频值 = (200 MHz + 8·baud) / (16·baud)。
- 只接受 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600。
- **921600 误差 −3.1%，不要用。** 相机支持的最高档 460800 误差 +0.47%。
- 相机上电默认波特率按 Camera Link 惯例应是 9600，待实测。VisionKit 枚举相机时会逐个波特率尝试（`EnumCameraByBaudRate`）。

## 4. GenCP 帧 [逆向]

完整字节表见逆向记录 2.4 节。要点：
- 全部字段大端。
- 读命令 `0x0800`，flags `0x4001`；写命令 `0x0802`，flags `0x4000`。
- 应答命令 `0x0801` / `0x0803`。相机忙时先回 `0x0805`（PendingAck），按它给的毫秒数延长等待。
- 校验和是 16 位反码求和，不是 CRC16：
  - CCD 校验和覆盖字节 6..15；
  - SCD 校验和覆盖字节 6 到包尾。
- 应答的状态码必须为 0，请求号必须和请求一致；VisionKit 不检查应答的校验和，本仓库也不检查。
- 相机寄存器值是大端 32 位（相机 XML `BigEndian`）。

## 5. 驱动白名单（`board/driver/sg3000_rx.c`）

- **默认关闭。** 新 `.ko` 不带参数加载时，行为和旧驱动完全一样。
- 加载时写 `insmod sg3000_rx.ko cam_serial=1`，才放开下表这些地址：

| 方向 | 地址（只有端口 0；端口 1 已删，见第 1 节）| 允许的值 |
|---|---|---|
| 写 | 0x411004 | 只能 0x04000000 |
| 写 | 0x411008、0x411018、0x411028 | 只能 0xa5 |
| 写 | 0x411010 | 任意 |
| 写 | 0x411014 | 17..528 |
| 读 | 0x411004、0x41101c、0x411020、0x411024 | — |
| 写 | BAR0 0x1000、0x1004、0x100c| ≤ 0xff |
| 读 | BAR0 0x100c| — |

白名单逻辑由 `board/tests/driver_whitelist_test.c` 在电脑上编译测试（`make -C board test`）。
`.ko` 本身要用板子的内核树编译（`board/driver/module.sh`），仓库里没有内核树，这一步还没做。

## 6. 已证伪

| 说法 | 结论 |
|---|---|
| 波特率分频"250 或 50" | ❌ 那是 `virtualserial.dll` 里的毫秒超时 |
| 用 JAI ASCII 短命令（`WN`、`LR`、`SBDRT`……）配置相机 | ❌ VisionKit 全部二进制 0 命中 |
| GenCP 串口帧用 CRC16 | ❌ 是 16 位反码求和 |
| 分频寄存器可能在 BAR1 | ❌ 在 BAR0（第 3 节）|

## 7. 短期替代（不写代码）

~~在 Windows 上用 VisionKit 把相机设好。~~ 手边没有能插 PCIe 卡的 Windows 电脑，这条走不通；改走第 9 节，从相机本身读 XML。

## 8. 待办（按顺序）

### 8.1 编译新 `.ko`（Windows 电脑上的 Linux 编译环境）

材料已经在你电脑上，见 `docs/legacy/使用说明.md` 第 49 行：
- 内核树和编译环境：`D:\CodexData\visionkit_zcu102\linux_build_vm`（linux-xlnx，提交 `b0c1be30…`）
- 板子内核的 Image 和 symbols.json：`D:\CodexData\visionkit_zcu102\boot_2022_1\kernel_analysis`

在那个 Linux 环境里，仓库根目录下运行：

```
KDIR=<准备好的内核树> KERNEL_ANALYSIS=<kernel_analysis 目录> CROSS_COMPILE=aarch64-none-elf- sh board/driver/module.sh
```

- 必须看到 `Compile and target ABI checks passed.`；ABI 检查不过，就不要上板。
- 打包：`python tools/make_sd_bundle.py --ko board/driver/output/module/sg3000_rx.ko`。这样 `.ko` 会进 `dist/glass-capture/` 和 `SHA256SUMS`，中控台会优先加载它。

### 8.2 上板实测结果（2026-11-14，详见 `docs/bringup_report_2026-11-14.md`）

1. **新驱动不带参数**：✅ 通过。`02 → 02½ → 03 → 04` 正常采到 `4096 × 2000` 帧（8,192,000 B），与旧驱动一致。
2. **开串口白名单**：✅ `cam_serial=1` 加载后 `/sys/module/sg3000_rx/parameters/cam_serial == Y`。
3. **`camera-probe`**：✅ `--baud 115200`（`port=0`）读出 `CameraManufacturer=JAI`、`CameraModel=SW-WS2-CL16K-50M-SDE`。
4. **其他参数测试与硬件注意**：`9600` 返回 `-110`，`19200 / 38400 / 57600 / 460800` 返回 `-71`；**在当前单路卡（`camera enum = 0x30424630`）上运行 `--port 1` 会触发 `nwl-pcie: Slave error` / `synchronous external abort` 导致系统异常**，在加驱动/应用端拦截前切勿探测 `--port 1`。
5. 恢复：重启板子即可（或 `rmmod sg3000_rx && insmod /tmp/sg3000-board/sg3000_rx.ko`）。

### 8.3 之后

- [x] 16K 相机的 XML：不需要 Windows，从相机本身读（第 9 节，`camera-xml`）。**待上板。**
- [ ] 用 16K XML 核对 `cam_regs.h`，再启用 `cam_config`（写相机）。
- [x] 单路卡拦截 `port 1`：用户态读 camera enum 拒绝；驱动白名单去掉端口 1（需重编 `.ko`）。

## 9. 从相机本身读 XML（GenCP 清单表）[标准 + 逆向]

GenCP 标准规定相机必须把自己的 GenICam XML 存在设备里，并在 BRM 里给出"清单表"。
VisionKit 读"设备 XML"走的就是这条路（`VisionKitApi.dll` `0x1800469b0..0x180047176`，日志串 `read device xml start/ok`），所以 Windows 电脑不是必需的。

| 步骤 | 读的地址 | 长度 | 出处 |
|---|---|---|---|
| 清单表地址 T | BRM `0x01D0` | 8，大端 | `0x1800469c4` 立即数 `0x1d0`、长度 8 |
| 条目数 | `T` | 8，大端 | `0x180046a83` |
| 第 0 条 | `T + 8` | VisionKit 读 0x18；本仓库读整条 64 | `0x180046ad4`（`lea 0x8(%rbx)`、长度 `0x18`）|
| 文件类型 | 条目 +4 的 bit10..15 | 0 = `.xml`，1 = `.zip` | `0x180046c4d`（`shr $0xa; and $0x3f`）|
| XML 本体 | 条目 +8 的地址 | 条目 +16 的大小 | `0x180047176`，一次调用，内部按块读 |

条目 64 字节：文件版本 4 | 格式版本 4 | 地址 8 | 大小 8 | SHA1 20 | 保留 20（GenCP 标准；VisionKit 用前 24 字节，与之一致）。

- VisionKit 每块读 `0x210 − 0x10 = 512` 字节（`0x180029289`、`0x18002a452`）。本仓库保守用 256 字节一块，失败的块最多重试 3 次，重试前清掉串口残留。
- 115200 波特下大约 9 KB/s：100 KB 的 zip 约 12 秒。
- 相机在清单里给了 SHA1 时，中控台在板上跑 `sha1sum` 核对，不一致就报错重读。
- 文件名：`<型号>_<主>.<次>.<小><.zip|.xml>`，存在板上 `/tmp/sg3000-board/camera-xml/`，取回电脑 `D:\SG3000_Captures\camera-xml\`。
