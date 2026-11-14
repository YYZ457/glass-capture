# 架构

状态：✅ 实机日志证明 ｜ 🟡 写了但没上板跑过 ｜ ⬜ 空，待写 ｜ ❓ 缺外部信息（厂家资料）

## 1. 全链路

```
相机 SF6200-CL16K（出厂默认配置）
  │ Camera Link Base ················ I1
SG3000 卡（块先存在卡上 DDR）
  │ PCIe / XDMA，每次 64 KiB，轮询 ·· I2 寄存器、I3 块头
ZCU102 · ARM · Linux 5.15（PL 不在通路里，D001）
  driver/ ──ioctl── I4
  capture/ ── I5 ── stream/ ── processing/ ── transport/
  │ 现在：USB 串口 115200；计划：千兆网 ·· I6
电脑 host/
```

满速约 252 MB/s，原图出不了板子，最终要板上检测（D002）。

## 2. 依赖方向

只能从上往下调用：

```
apps ─▶ stream ─▶ processing
  │        │
  │        └────▶ capture ─▶ (sg_capture_io 回调表) ─▶ capture/sg_io_linux.c ─ioctl─▶ driver
  └─▶ transport、camera_ctrl ─▶ capture/sg_session
```

`host/` 不认识任何寄存器，只认 I6 和 `docs/interfaces/schemas/`。

## 3. 文件清单

### include/ — 跨模块共享

| 文件 | 职责 | 状态 |
|---|---|---|
| `sg3000_regs.h` | **所有**寄存器地址和魔数，每条标可信度 | ✅ 已从源码抽出 |
| `sg3000_rx_uapi.h` | 驱动 ⇄ 用户态的 4 个 ioctl | ✅ 原样 |
| `sg_capture_io.h` | 协议层向下的回调表 | ✅ 原样 |
| `sg_frame.h` | 采集块结构体 | ✅ 原样 |
| `sg_err.h` | 每个错误码在本项目里的含义 | ✅ |
| `sg_pl_uapi.h` | PL 驱动 ABI（提议，无实现）| 🟡 |

### board/driver/ — 只搬字节，不认识相机

| 文件 | 职责 | 状态 |
|---|---|---|
| `sg3000_rx.c` | PCIe 绑定、BAR 映射、寄存器白名单、DMA、出错隔离 | ✅ 2026-11-14 已编译并通过 12382 符号 ABI 检查；实机 `cam_serial=0` 采图与旧驱动一致，`cam_serial=1` 已打通 port0 `camera-probe`；白名单已去掉端口 1（单路卡碰它会 PCIe Slave error），**需重编 .ko**。见 `docs/bringup_report_2026-11-14.md` |
| `Kbuild`、`module.sh`、`verify_module.py` | 构建 .ko 并检查目标内核 ABI | ✅ 仅改路径 |

### board/capture/ — 和卡的固件对话（原 `sg3000_capture_core.c` 拆成 5 份）

| 文件 | 职责 | 状态 |
|---|---|---|
| `sg_session` | 会话、超时参数、读写小工具、帧释放 | ✅ 实机单帧与读 XML 验证通过 |
| `sg_mailbox` | 邮箱报文收发 | ✅ 实机验证通过（严格寄存器读 + 存储块读 `sg_mbx_read_block` + 卡参数写 `sg_mbx_write_value`：`CameraWidth` `4096 -> 16384` 读回一致且断电恢复默认） |
| `sg_preflight` | 只读体检 | ✅ 实机验证通过 |
| `sg_block` | 块头读取、校验（纯函数）、分块 DMA | ✅ 实机单帧采集验证通过 |
| `sg_acq` | 开始 / 取块 / 停止状态机，单帧便捷函数 | ✅ 实机单帧采集验证通过 |
| `sg_cardxml` | 从卡存储读出卡自己的 GenICam XML（URL 在逻辑地址 0x190）| ✅ 实机读出 `SG3000V100R001C00XB046.zip`（38,217 B，每包固定 `0x100` 请求） |
| `sg_io_linux` | 回调表的 ioctl 实现（原来两个程序各抄一份，现合并）| ✅ 实机验证通过 |

### board/processing/ — 一块像素进，一个结果出

| 文件 | 职责 | 状态 |
|---|---|---|
| `sg_processing.h` | 处理器接口 | ✅ 原样 |
| `sg_view.c` | 输入合法性检查 | 🟡 已迁移，电脑上测过 |
| `sg_stats.c` | CPU 诊断统计（不是缺陷判定）| 🟡 已迁移，电脑上测过 |
| `sg_pl_backend.c` | PL 处理器 | 🟡 暂不迁移（D001）|
| `sg_candidates` | 颗粒候选检测（第一版算法）| 🟡 已写，和 `host/detect/candidates.py` 输出一致；没对过真值 |

### board/stream/

| 文件 | 职责 | 状态 |
|---|---|---|
| `sg_pipeline` | 采集线程 → 有界队列 → 处理线程 | 🟡 已迁移，合成数据测过，没上板 |
| `sg_linecheck` | 块号连续性、累计行数、实际行频 | 🟡 已写，电脑上测过（还没接进程序）|

### board/camera_ctrl/ — 相机控制（I7）

| 文件 | 职责 | 状态 |
|---|---|---|
| `cam_regs.h` | 相机寄存器地址 | ✅ 已用从相机读出的 16K XML 逐条核对（更正波特率地址、时钟 96 MHz）|
| `cam_serial` | 串口邮箱初始化 / 收发 / 波特率 + GenCP 读写 | ✅ 实机 `cam_serial=1` + `port=0, baud=115200` 读出 `JAI / SW-WS2-CL16K-50M-SDE`；单路卡禁碰 port1（见 `docs/bringup_report_2026-11-14.md`） |
| `cam_xml` | GenCP 清单表 → 从相机本身读 GenICam XML；按块读内存（重试 + 清残留）| ✅ 实板读出相机 XML（18,279 B，2 s）|
| `cam_set` | `camera-set` 白名单 + 换算（CL 模式、Tap、测试图、曝光、数字/模拟增益）| 🟡 单测通过；CL 模式 / Tap 已实板写通，其余待测 |
| `cam_config` | 宽度、行频、曝光、触发、测试图案、User Set | 🟡 已写在 cam_serial 之上；地址待 16K XML 核对 |

### board/transport/

| 文件 | 职责 | 状态 |
|---|---|---|
| `tx_jsonl` | 结果写成 `SGJSON` 行到 stdout（经串口给中控台）| 🟡 已迁移，输出能被 telemetry.py 解析 |
| `tx_tcp` | 经网口发原图块 / 候选 / 状态（I6）| 🟡 已写，本机回环和 rx_tcp 对通，SHA256 一致 |

### board/apps/ — 只做参数解析和组装

| 文件 | 程序 | 状态 |
|---|---|---|
| `sg_status.c` | `sg_status` 只读体检；`--card-xml DIR` 读卡 XML | ✅ 实机体检与 `--card-xml` 读出均通过 |
| `sg_capture_main.c` | `sg3000-capture` 单帧；`send` 经网口发图；`card-set` / `camera-set`（白名单 + 读回）；`camera-probe` / `camera-xml` / `camera-regs` / `camera-read`（只读）；失败时打印 `FailedStage` / `LastHeader`；`version` | ✅ `preflight` / `capture`（4096×2000 及 Base 1X3 `16384×2000`）/ `version` / `card-set` / `camera-set` / `camera-probe`（port0）实机全部通过 |
| `sg_stream_main.c` | `sg3000-stream` 连续；`--processor candidates`、`--tcp` | ✅ 合成源与真机 `--source hardware --processor stats`（`16384×2000`，`stop_error=0`）均实机通过 |
| `pgm_write` | 原子写 PGM | 🟡 已写，电脑上测试通过 |

### 其他

| 目录 | 内容 | 状态 |
|---|---|---|
| `host/console/`、`host/tests/` | 中控台（Python 网页服务 + 串口通道 + 网口接收 + 离线检测）及测试 | 🟡 已改为部署本仓库程序、去掉二进制补丁、加网口；模拟板子端到端测过 |
| `host/receiver/rx_tcp.py` | I6 接收端（命令行 + 中控台内置）| 🟡 已写，和 tx_tcp 对通 |
| `host/detect/candidates.py` | 候选检测 Python 参考（和板上 C 同算法）| 🟡 已写 |
| `fpga/` | 三个 HLS/RTL 包，原样保留，不细分（D001）| 🟡 |
| `verify/V1-V4` | 测试图案、行完整性、暗场、真值比对 | 🟡 已写，合成图测过；V1 渐变公式待手册确认 |
| `board/tests/` | 假卡测试、单元测试（`make -C board test`，x86 和 ARM64 模拟器都过）| 🟡 |
| `tools/make_sd_bundle.py` | 交叉编译 → `dist/glass-capture/`（拷到 SD 卡）| 🟡 |
| `dist/glass-capture/` | 编译好的 ARM64 板上程序 + SHA256SUMS | ✅ 已上板校验并运行 |
| `docs/BRINGUP.md` | 上板步骤 | — |
| `platform/` | 启动文件 | ⚠️ 只在 SD 卡上 |
| `legacy/` | 原始代码，只读，迁移参照 | ✅ 仍可编译 |
| `docs/vendor/` | 厂家 SDK 头文件、示例源码、同系列相机 XML、外部报告（带核对表）| ✅ 2026-11-07 收入 |

## 4. 填写顺序

1. `sg_session` → `sg_mailbox` → `sg_preflight` → `sg_io_linux` → `apps/sg_status`：第一个能上板跑的新程序，只读零风险
2. `sg_block` → `sg_acq` → `pgm_write` → `apps/sg_capture_main`：和 legacy 采同一帧，逐字节对比
3. 去掉二进制补丁（D003）
4. `verify/V2`、`V3`：现在就能做，不需要镜头
5. Windows + VisionKit 一趟：填 I1、I2 待办、I7，跑 V1
6. `tx_tcp` + `rx_tcp`：告别串口
7. `sg_linecheck`、`sg_pipeline`、`apps/sg_stream_main`：连续采集上板
8. 检测：host 上跑通 + V4，再写 `sg_candidates`

每一步：迁移 → `make -C board check` → 上板对比 legacy → 更新本文件状态列。
