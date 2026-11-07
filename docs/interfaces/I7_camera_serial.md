# I7 · 相机控制（GenCP over Camera Link 串口）

## 已知

- 相机在 VisionKit 里是独立的 **GenCP 设备**，和采集卡分开连接（`DeviceTypeGencp = 0x1`）
- GenCP 是 EMVA 标准的二进制读写协议：按地址读写寄存器，寄存器表来自相机自己的 GenICam XML
- 相机寄存器地址：`board/camera_ctrl/cam_regs.h`（全部 [参考8K]，见该文件说明）
- 串口波特率可选 9600 / 115200 / 460800（寄存器 0x04）
- Windows 上可用 `VkReadAddressValue` / `VkWriteAddressValue` 直接按地址读写，用来核对 cam_regs.h

## 未知（写 board/camera_ctrl/cam_serial.c 之前必须解决）

| 问题 | 怎么查 |
|---|---|
| Linux 下怎么经 SG3000 卡收发串口字节 | 卡的 XML / 厂家；外部报告说走 XDMA 另外两个通道，**无出处** |
| GenCP 帧在串口上的具体封装 | GenCP 标准文档（EMVA 官网公开）|
| 16K 相机的寄存器地址是否和 8K 相同 | 16K 相机自己的 XML |
| JAI ASCII 短命令是否可用 | 外部报告提到，**无出处**；可在 Windows 串口终端发 `ID?` 试 |

## 短期替代（不写代码）

Windows 上用 VisionKit 设好相机（宽度、行频、曝光、CL 配置），存入 User Set 并设为上电默认（`UserSetSave` / `UserSetDefault`）。
相机之后每次上电自动用这套参数，Linux 端暂时不需要串口。
