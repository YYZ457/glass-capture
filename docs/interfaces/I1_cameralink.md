# I1 · 相机 → 采集卡（Camera Link）

## 相机

| 项 | 值 | 来源 |
|---|---|---|
| 铭牌 | JAI `SW-WS2-CL16K-50M-SDE`，No. `SF1AJFF12602` | 铭牌 |
| 系统内名称 | 凌云光 `SF6200-CL16K-50M`（SF6000 系列）| 采购记录 |
| 传感器 | 16384 像素（公开同款为 2 线），3.5 µm | JAI SW-16001M-MCL 公开资料 |
| 最高行频 | 50 kHz | 同上 |
| 供电 | 12–24 V ≈1.2 A（约 15 W）| 铭牌 |
| 控制协议 | GenCP，经 Camera Link 串口 | VisionKit SDK |

## 链路配置

| 项 | 可选值 | 现在 | 来源 |
|---|---|---|---|
| CL 配置 | **只有 Base（单线）或 EightyBit（双线，10 tap）**，没有 Medium / Full | 卡报告 Base | 相机 XML [参考8K] |
| tap 数 | 1 / 2 / 3 / 10 | 未知 | 相机 XML [参考8K] |
| CL 像素时钟 | 85 / 80 / 66 / 40 MHz | 未知 | 相机 XML [参考8K]；与 JAI 公开资料一致 |
| 像素格式 | Mono8（0x01080001）| Mono8 | 观察 |
| 相机输出宽度 | 寄存器 0x01303040 | 未知（出厂默认）| 相机 XML [参考8K] |
| 卡接收宽度 | 卡参数 `CameraWidth`，地址未知 | **4096**（Linux 从未设置）| 示例源码 + 观察 |

## 带宽

见 D002。结论：Base 3-tap 85 MHz 理论 255 Mpx/s，75 mm/s 需要 252 Mpx/s，每行只剩约 71 个时钟做行消隐，**单线跑满 75 mm/s 基本不现实**。
可选：降到约 60 mm/s（≈201 Mpx/s），或上 EightyBit 双线（需确认卡支持且有第二根线）。

## 怎么确认

1. Windows + VisionKit 连一次相机，导出它自己的 XML（见 `docs/vendor/README.md` "还缺"）
2. 在 VisionKit 里读出当前 CL 配置、tap、时钟、宽度
3. 打开测试图案（寄存器 0x01100000 = 3，水平灰度渐变）采一帧，跑 `verify/V1`
