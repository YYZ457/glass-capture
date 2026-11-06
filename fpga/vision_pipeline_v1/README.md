# 可组合视觉模块 v1

用途：给 ZCU102＋SG3000 项目留下可运行、可测试、接口固定的基础块。没有绑定训练模型。先运行离线示例，再替换权重、模型图和板级后端。这里没有真实相机采图成功、整网识别准确率或实时吞吐的承诺。

## 立即运行

在本机 PowerShell 中，从本目录运行：

```powershell
.\verify.ps1
& 'C:\Users\YANYZ\anaconda3\python.exe' -X utf8 .\examples\offline_pipeline.py `
  --output-dir D:\CodexData\visionkit_zcu102\reusable_core\my_demo `
  --native-library D:\CodexData\visionkit_zcu102\reusable_core\verify\vision_quant.dll `
  --threshold 15
```

输出目录必须尚不存在，以保护已有图像。增加 `--input 完整帧.pgm` 可直接读取 `sg3000-capture` 成功结束后发布的 Mono8 PGM。未加 `--input` 时使用明确标记的合成图；示例权重只用于校验组合，不是训练模型。结果里的 `peak` 是灰度响应，绝不是缺陷概率。局部背景滤波可能在强亮点周围产生暗环候选，必须继续筛选/复核。

## 模块选择

|模块|已实现的功能|入口|
|---|---|---|
|传统数值基础|dark/flat 校正、有效区域、整数均值去背景、亮/暗/双向响应、阈值、8邻域连通域、ROI、明确的 nearest 缩放|`python/traditional.py`|
|旧低对比检测|冻结的多尺度 DoG＋局部噪声估计＋相邻尺度支持|`python/legacy_adapter.py`|
|通用量化运算|普通/深度卷积1×1和3×3、2×2池化、逐通道重定标＋可选ReLU、INT32饱和加法|`hls/quant_engine.cpp`|
|CPU组合运行|同一份HLS C++编译成DLL，具名Job与word arena，实际逐算子调用|`python/quant_runtime.py` / `hls/native_bridge.cpp`|
|硬件任务生命周期|64位DMA地址、完整参数快照、清旧done、单次启动、等待、同步、超时隔离、复位排空后恢复|`HlsExecutor`＋显式Backend接口|
|流缓冲|同步FIFO，任意深度≥2，满时同时进出，背压保持数据/标记|`rtl/stream_fifo.sv`|
|灰度帧标记|尺寸快照、SOF/EOL/EOF、背压稳定、末像素被消费才完成|`rtl/gray_frame_framer.sv`|
|自定义RTL任务控制|请求/响应握手、配置快照、标签、完成、deadline、abort、超时后禁止复用|`rtl/job_supervisor.sv`|

`job_supervisor` 用于自定义RTL执行器；HLS核本身使用工具生成的AXI-Lite与 `ap_ctrl_hs`。不要把两套启动信号直接并接，若组合使用须明确适配层和唯一控制者。

## 冻结的数据合同

- 相机入口先限定完整、连续的 **Mono8二维图**。SG3000原始包继续由现有 `tools/sg3000_linux` 严格组帧；不能绕过包序号/偏移/帧长/停止确认检查。这里不声称修复相机或已收到真实DMA图像。
- 像素坐标是原图坐标，bbox=`[x0,y0,x1,y1)`，右/下边界不包含。ROI必须同时保留原框与缩放规则。
- HLS张量是 **CHW**，普通卷积权重 **OIHW**，depthwise权重 **C1HW**，depth multiplier固定1。
- 每个INT8元素占一个32位word，低8位是二进制补码，高位读取忽略、写出清零；INT32使用完整word。**所有offset单位是word；AXI-Lite寄存器地址单位是byte。** 不要擅自改为四像素打包。
- 激活/权重均是有符号对称INT8；padding数值为0；没有非零zero-point。摄像头Mono8不能直接视为INT8量化张量。示例减128仅为演示，真实模型按自己的标定合同转换。
- 卷积完整INT64累加后才饱和到INT32。重定标=`round_ties_away(input*multiplier/2^shift)`，multiplier在0..INT32_MAX，shift在0..31，再INT8饱和，最后可选ReLU。负数半值向远离0方向舍入。
- C_in/C_out≤64、H/W≤128、kernel=1/3、stride=1/2、pad=0/1。池化固定2×2/stride2，奇数最后一行/列丢弃。REQUANT/ADD形状不变。
- 参数/range/输出别名/scale全部验证后才写输出；拒绝时整块内存不变。输出不能覆盖任何输入/权重/scale范围；只读范围之间可重叠。
- 算子的unused维度/卷积参数/flag必须为0；unused offset忽略。精确定义看 `hls/quant_engine.h`。
- `memory_words`≤2^24 是调用者声明的真实分配大小；硬件不能探测物理内存是否合法。HLS pragma `depth=4096` 仅给RTL协同仿真建模，不是硬件上限。

不支持的模型算子必须显式报错或走经验证的CPU后端，不能静默近似。当前没有自动ONNX转换、非对称量化、任意groups卷积、SiLU、softmax、NMS或完整YOLO。旧 `D:\glassgate_7020` W8A16路径也不能直接套用本模块INT8合同。超过64输入通道不能简单分段饱和后相加来冒充一次完整卷积。

## 明场与暗场

`BoxConfig(polarity='bright'|'dark'|'both')` 显式选择响应。换照明后重新采集dark/flat和无缺陷样本，确定有效区域、阈值、最小面积及需要保留的缺陷类型；不要仅凭“暗场”就永久丢弃暗响应。

旧代码来自 `D:\glass_defect_trial\20260908_lowcontrast\weak_detector.py`，原样复制到 `legacy/weak_detector_20260908.py`，SHA256：

`02a2c4c2e101cf786226fac3b718960b82086ce64bc03a543526645f7aea6c5f`

适配入口只调用 `detect_tile` 和 `consolidate`，不调用旧 `geometry()` 的白边>220推断，不做旧全幅列纹扣除，也不依赖旧JSON中的检测框。旧算法的DN阈值和噪声估计仍需新照明验证。它额外依赖OpenCV、SciPy；新基础模块只依赖NumPy。

`tiled_box_response` 的halo至少等于radius，拼接不重叠core后再做全局连通域，因此跨tile分量不会被截断。它保留全图输出，仍消耗RAM；Python连通域与旧浮点DoG均未承诺16K线扫实时速度，也不是已经移植到PL的预处理核。

## 时钟、复位与超时

- 三个RTL模块均为单时钟、同步高有效复位。它们不解决CDC；跨时钟必须使用单独验证的异步FIFO/复位同步模块。
- ready/valid仅在时钟沿同时为1时传输。上游在valid=1且ready=0时必须保持数据/sideband。FIFO满时有 `m_ready→s_ready` 组合路径，系统连接不能形成组合环。
- framer接收精确W×H像素，末输出真正握手后才 `frame_done`；输入停顿不会改变计数。它无法发现来源丢了一个像素；相机采集层仍负责完整性。
- supervisor的 `engine_done` 必须是当前任务的新完成脉冲，`engine_status` 同拍有效；不能直连未经处理的旧sticky done。配置从engine_start开始保持到下一次请求。
- supervisor timeout从engine看到start的边沿开始计数；同拍优先级abort > done > deadline。0 timeout直接拒绝。返回码fffffffd/fffffffe/ffffffff预留给参数错/abort/timeout。
- 超时只表示“软件不再等待”，**不表示AXI写已停止**。隔离时禁止释放、改写、复用DMA缓冲区。`recovery_ack` 只有在外部实际复位/停止执行器并确认所有总线事务排空后才能给出；idle信号本身不够。
- supervisor复位必须与执行器/互连协调，否则会忘记仍在途的事务。软件异常（包括不确定是否已生效的启动写）同样进入隔离。

## ZCU102接入只在这些边界做适配

1. 先恢复现有Linux/SG3000诊断，确认采集链路。相机暂时不可用不影响本目录离线测试。
2. 在ZCU102 Vivado工程加入生成的HLS IP，控制AXI-Lite接PS，64位地址的m_axi接可访问DDR的端口；明确时钟、复位、地址窗口。不要使用旧AX7020的固定地址或约束。
3. 实现 `Backend`：分配并拥有本执行器可访问的DMA缓冲区，提供正确的DMA地址/IOVA，MMIO访问顺序与缓存同步，可靠的reset-and-drain。**Python数组指针和CPU虚拟地址都不是DMA地址。**
4. SG3000的PCIe设备DMA地址不能假定可直接供PL使用；IOMMU域/缓存属性可能不同。第一版建议受控复制到PL自己的分配中，零拷贝需另行实现跨设备映射和生命周期。
5. 用同一份小张量先验证写回、负数、饱和、连续两次任务和故意超时恢复，再接ROI和模型权重。只有实板逐元素匹配后才将结果标记board_passed。

本包未提供 `/dev/mem`、未经确认的寄存器物理地址或自动下载bitstream；这些必须根据最终硬件设计和Linux驱动实现。`HlsExecutor` 是可测的控制逻辑＋后端协议，并非已经完成的Linux PL驱动。

## 构建和验收

`verify.ps1` 实际运行C++独立整数参考测试、Python组合/异常路径/预处理/文件测试、三套RTL仿真，并注入FIFO故障确认runner会判失败。XSim/HLS某些错误仍退出0，必须同时检查日志、PASS标记和生成文件。

HLS单独构建使用 `build-hls.ps1 -Mode synth` 或 `-Mode cosim`，成功后 `export-ip.ps1` 导出同一工程的 `quant_engine_ip.zip`。解压到独立IP仓库后，在Vivado的IP Repository中添加。目标器件xczu9eg-ffvb1156-2-e、目标时钟10ns。cosim使用独立4096word小测试，覆盖连续7个作业与拒绝路径；全量随机数值测试由verify/csim完成。

综合估计不等于布线时序通过，更不等于板测。当前卷积核优先保证通用参数和数值语义，未做片上分块缓存/大规模MAC并行优化；实时吞吐需要根据最终模型、输入速率和DDR带宽设计下一版，并保持本版黄金输出。

给Gemini的修改边界见 `AGENTS.md`；寄存器和数值合同见 `contract.json`；当前构建产物与日志路径见 `validation.json`。每次修改都重新运行相应验证，不继承旧PASS。
