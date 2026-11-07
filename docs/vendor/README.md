# vendor · 厂家原始资料（2026-11-07 收入）

来源：`VisionKit_SG3000_SF6200_Source_Evidence.zip`。**私有仓库，仅供本项目参考，不要公开。**
头文件和示例源码原为 GBK 编码，已转成 UTF-8，内容未改动。

| 目录 | 内容 | 核对结论 |
|---|---|---|
| `visionkit_demos/` | 凌云光 VisionKit 官方示例：Qt、MFC、控制台三个 | ✅ 真实。采集卡参数名都在这里（见下）|
| `visionkit_sdk/` | SDK 头文件 + 3 页开发示例 PDF | ✅ 真实。API 列表见 `VkApiDefination.h` |
| `genicam_xml_8k_reference/` | 3 份凌云光 SF 系列相机 GenICam XML | ✅ 真实，但**都不是我们这台**：SF6200-**CL08K**、SF6100-CL08K、SF3000-CL04K，传感器 8192 / 4096 宽 |
| `agent_report_unverified.md` | 另一个 agent 写的集成报告 | ⚠️ 部分对、部分无依据，见文件开头的核对表 |

## 关于相机的身份

- 铭牌：**JAI**，型号 `SW-WS2-CL16K-50M-SDE`，序列号 `SF1AJFF12602`，供电 12–24 V ≈1.2 A
- 采购 / 系统里的名字 `SF6200-CL16K-50M` 是凌云光（LUSTER）SF6000 系列的命名
- 两者应为同一台：JAI 制造，凌云光型号销售（凌云光收购了 JAI，待供货方确认）
- 最接近的公开型号：JAI SW-16001M-MCL（16384 ×2 线，3.5 µm，50 kHz，CL 时钟 40/66/80/85 MHz）

## 从这些资料里确认的事

**采集卡（来自示例源码）**：Windows 软件连接后会对卡设置这些参数（GenICam 名字）：
`ClConfiguration`、`CameraColorType`、`CameraPixelDepth`、`CameraScanType`、`DeviceTapGeometry`、
**`CameraWidth`**、**`CameraHeight`**、`CL_CC1`、`LineTriggerEnable`、`FrameTriggerEnable`、
`LineTriggerSource`、`FrameTriggerSource`、`LineInternalTriggerRate`，并调用 `SupportVariableFrame(1)`。
我们的 Linux 代码一个都没设过 → 卡报告 4096 宽的直接原因。**这些名字对应的寄存器地址仍然未知。**

**相机**：在 SDK 里是一个独立的 "GenCP" 设备（`DeviceTypeGencp = 0x1`），经 Camera Link 串口通信。
SDK 会从相机本身"下载 XML"（`VkSetMaxBaudrateDownXmlSupport`），说明 16K 相机的 XML 存在相机里。

**SDK 里两个以后很有用的函数**：
- `VkConfigSave(设备, 文件名)`：把相机或采集卡的当前全部参数存成文件
- `VkReadAddressValue(设备, 缓冲, 地址, 长度)`：读任意地址，可用来在 Windows 上验证寄存器

## 还缺

1. **16K 相机自己的 XML**。线索：这 3 份 XML 的文件名格式（`SchemaVersion.1.1@Luster#型号#版本@XMLVersion...xml`）像是 VisionKit 的缓存文件名。
   在 Windows 电脑上用 VisionKit 连一次 16K 相机，然后全盘搜 `SchemaVersion*CL16K*`，很可能就有了。
2. **采集卡的 XML**（报告称为 `SGCard.xml`，存在卡的 Flash 里，未核实）：`CameraWidth` 的真实地址在里面。
3. 两个设备的 `VkConfigSave` 输出文件：当前参数的完整快照。
