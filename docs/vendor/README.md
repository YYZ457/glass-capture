# vendor · 厂家原始资料（2026-11-07 收入）

来源：`VisionKit_SG3000_SF6200_Source_Evidence.zip`。**私有仓库，仅供本项目参考，不要公开。**
头文件和示例源码原为 GBK 编码，已转成 UTF-8，内容未改动。

| 目录 | 内容 | 核对结论 |
|---|---|---|
| `visionkit_demos/` | 凌云光 VisionKit 官方示例：Qt、MFC、控制台三个 | ✅ 真实。采集卡参数名都在这里（见下）|
| `visionkit_sdk/` | SDK 头文件 + 3 页开发示例 PDF | ✅ 真实。API 列表见 `VkApiDefination.h` |
| `genicam_xml_8k_reference/` | 3 份凌云光 SF 系列相机 GenICam XML | ✅ 真实，但**都不是我们这台**：SF6200-**CL08K**、SF6100-CL08K、SF3000-CL04K，传感器 8192 / 4096 宽 |
| `agent_report_unverified.md` | 另一个 agent 写的集成报告 | ⚠️ 部分对、部分无依据，见文件开头的核对表 |
| `reverse_engineering_2026-11-14.md` | 反汇编 XDMA.sys / VisionKit DLL 的结论 | [逆向]：第 1 部分 Gemini，第 2 部分 Claude 对着 `windows_bin/` 逐条复核和补充 |
| `windows_bin/` | Windows 端原始二进制（XDMA.sys + PDB、virtualserial、LusterGraber、VisionKitApi 等）和 16K 相机彩页 PDF | ✅ 原文件，SHA256 与 Gemini 记录一致。**厂家文件，不得公开** |
| `card_xml/` | 2026-11-14 从 ZCU102 实板 `sg_status --card-xml` 读出的采集卡 XML（`SG3000V100R001C00XB046.zip` 及解压后的 `.xml`） | ✅ 实机读出，**采集卡端寄存器已实锤**（`CameraWidth = 0x10203008` 等）。**私有仓库保留，不得公开** |

## 关于相机的身份

- 铭牌：**JAI**，型号 `SW-WS2-CL16K-50M-SDE`，序列号 `SF1AJFF12602`，供电 12–24 V ≈1.2 A
- 采购 / 系统里的名字 `SF6200-CL16K-50M` 是凌云光（LUSTER）SF6000 系列的命名
- 两者应为同一台：JAI 制造，凌云光型号销售（凌云光收购了 JAI，待供货方确认）
- 最接近的公开型号：JAI SW-16001M-MCL（16384 ×2 线，3.5 µm，50 kHz，CL 时钟 40/66/80/85 MHz）

## 从这些资料里确认的事

**采集卡（来自示例源码 + `card_xml/SG3000V100R001C00XB046.xml`）**：Windows 软件连接后会对卡设置这些参数（GenICam 名字）：
`ClConfiguration`（`0x10601004`）、`CameraColorType`（`0x10601008`）、`CameraPixelDepth`（`0x1060100C`）、`CameraScanType`（`0x10601000`）、`DeviceTapGeometry`（`0x10601010`）、
**`CameraWidth`**（`0x10203008`）、**`CameraHeight`**（`0x1020300C`）、`CL_CC1`、`LineTriggerEnable`、`FrameTriggerEnable`、
`LineTriggerSource`、`FrameTriggerSource`、`LineInternalTriggerRate`，并调用 `SupportVariableFrame(1)`。
我们的 Linux 代码之前一个都没设过 → 卡报告 4096 宽的直接原因。**采集卡端寄存器地址现已全部从 `card_xml/` 确认**；待新 `.ko` 驱动与写白名单/流程就绪后即可下发配置。

**相机**：在 SDK 里是一个独立的 "GenCP" 设备（`DeviceTypeGencp = 0x1`），经 Camera Link 串口通信。
SDK 会从相机本身"下载 XML"（`VkSetMaxBaudrateDownXmlSupport`），说明 16K 相机的 XML 存在相机里。

**SDK 里两个以后很有用的函数**：
- `VkConfigSave(设备, 文件名)`：把相机或采集卡的当前全部参数存成文件
- `VkReadAddressValue(设备, 缓冲, 地址, 长度)`：读任意地址，可用来在 Windows 上验证寄存器

## 已补齐 / 还缺

- ✅ **已补齐（2026-11-14 实机读出）**：**采集卡的 XML**（`card_xml/SG3000V100R001C00XB046.zip` / `.xml`），`CameraWidth = 0x10203008` 等真实地址已在里面。
- ⬜ **还缺**：
  1. **16K 相机自己的 XML**。线索：这 3 份 XML 的文件名格式（`SchemaVersion.1.1@Luster#型号#版本@XMLVersion...xml`）像是 VisionKit 的缓存文件名。
     可以在 Windows 电脑上用 VisionKit 连一次 16K 相机全盘搜 `SchemaVersion*CL16K*`，或等 ZCU102 编译出带 `cam_serial=1` 的新 `sg3000_rx.ko` 后经 GenCP 串口读取。
  2. 两个设备的 `VkConfigSave` 输出文件：当前参数的完整快照（可选）。

## 16K 相机彩页（`windows_bin/SF6200-CL16K-50M-10 凌云光.pdf`）

产品宣传页，**没有寄存器表**。有用的事实：16384 × 2 像素、3.5 µm、50 kHz、Mono8 / 10bit、接口 **CameraLink Deca & Base**、
支持逐点 FFC、ROI、Binning、TDI、功耗 < 12 W。
满速 16384 × 50 kHz = 819 MB/s 只有 Deca 能传；Base（255 Mpx/s）在 16384 宽下最多约 15.5 kHz，正好够项目的 15.36 kHz，见 D002。
