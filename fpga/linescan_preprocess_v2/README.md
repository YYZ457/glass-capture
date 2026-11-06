# 16K 线扫 FPGA 预处理模块 v2

本模块实现 Mono8 列校正、局部背景估计、亮/暗缺陷响应和阈值筛选，并配齐 16K 条带缓存、横向分块和跨条带候选合并。交付包含实际导出的 ZCU102 HLS IP、可运行的 Windows 原生库、源码、独立参考、接口测试和 RTL 协同仿真证据。没有执行本版本的上板验证、真实暗场精度验证或端到端吞吐测试。

## 哪些直接放 FPGA

`hls/preprocess_tile.cpp` 是硬件核：列暗电平/增益校正 → 最大 15×15 方框背景 → 有符号差值 → 双极性/单极性阈值。内部采用 15 个分离行缓存、连续输入读缓存、256 像素输出行缓存和 AXI 突发写回。不是每个输出像素重读整个邻域。

`python/strip_cache.py` 和 `stream_pipeline.py` 在主机组织输入；`stream_components.py` 或 `cpu/stream_components.hpp` 在 CPU 合并候选。后者是独立的 C++11 单头文件，ARM Linux 无需 Python 即可复用。DoG、高斯、多尺度分类和 AI 算子仍见随包 v1；本核的方框背景不能替代它们的数学定义。不能承诺任意模型均能直接适配。

## 固定接口

顶层签名见 `hls/preprocess_tile.h`，机器可读值见 `contract.json`。一个 64 位地址、32 位数据的 `m_axi_ddr`，一个 AXI-Lite 控制口，单次 `ap_ctrl_hs`，禁止 auto_restart。

输入是 **4 个 Mono8 像素打包成一个 uint32，最低字节为左侧像素**。步长单位为字节且必须是 4 的倍数；三个 offset 单位均是 uint32 字，不是字节。此输入与 v1 AI 核“每个 int8 元素占一个 32 位字”的格式不同，不能直接共用指针布局。

每次计算至多 256×64 核心区，半径 0..7；源宽 1..16384，源高 1..65536。允许处理更长的逻辑帧：逐条带提交，保留全局 y。内存长度至多 2^29 字，参数、范围、写读重叠均在写入前检查。输出只含核心区，按行紧密排列。错误返回不修改内存。

| 输出位 | 内容 |
|---|---|
| 7:0 | 校正后的像素 |
| 15:8 | 局部背景 |
| 24:16 | 9 位补码响应，范围 -255..255 |
| 25 | 整个邻域有效 |
| 26 | 满足阈值 |
| 31:27 | 必须为零 |

响应解码：`int d=(word>>16)&511; if(d&256)d-=512;`。`-256` 非法。无效邻域的响应与命中均为零。背景仍是邻域数值之和的均值，不做有效像素数归一化。

校正表每列一个 uint32：最低 8 位是暗电平，最高 24 位是无符号 Q8.16 增益；增益零代表坏列。校正公式 `min(255,(max(raw-dark,0)*gain+32768)>>16)`。`reference.make_column_calibration` 从列暗/平场数据生成校正表。增益量化结果可能与 v1 精确除法相差 1 DN，两者合同独立。禁用校正时为恒等映射且有效。校正参数在整帧冻结。

背景采用方形窗口，只有真实图像边缘复制；全二维累加后一次四舍五入。polarity=0 双极性、1 亮、2 暗；threshold=1..255，含等号。暗场可先用 bright，反射明场可使用 both，但阈值和效果需要真实图像标定。

## 16K 分块和跨块规则

1. 输入碎片可以不是整行。`StripCache.push` 返回已接收字节数；调用者保留未接收尾部。ready 后背压，先 pop 再继续推送。禁止把返回 0 当作输入已消费。
2. 默认核心条带 64 行，上下各保留 7 行 halo。中间条带共 78 行；仅第一/最后条带按真实图像边界缩短。宽 16384 时缓存内部保留的输入载荷上限为 **1,277,952 字节**，不随帧高增长。这不含输出快照、DDR arena、Python 对象和临时拷贝，不是整个程序内存上限。
3. 水平方向使用 256 像素核心块，从同一全宽源条带读取左右 halo。核心区不重叠且覆盖整行。绝不在 x=256 或 y=64 的内部边界补零/复制边缘；只在真实帧边界复制。
4. `Stripe` 携带 frame_rows、halo_radius、source_y、core_y、frame_id。`StripeArena` 拒绝缺失 halo 元数据或半径不足的条带，防止错误拼接静默通过。
5. `process_chunks` 每次产出一个只读核心条带。消费者处理完再请求下一条带，不能用 list 收集整帧结果后声称内存有界。
6. 候选合并必须先恢复完整的全宽核心行，再按全局 y 顺序调用 `consume_row`，不能按 tile 顺序直接喂入。8 邻接、同极性合并，面积只计核心像素一次，跨条带不清空状态。帧结束调用 finish；新帧新建实例。
7. 输出 bbox 为半开区间，坐标和面积保留全局值。C++ 使用 uint64 y/area 并检查溢出。每行仅保留前沿 runs 与聚合统计，输出数量不截断，后续排序/存盘由调用方负责。

软件背压不能保证真实相机停止出数。采集驱动必须上报溢出、丢行和帧中断；不完整帧不可作为完整检测结果。本包提供碎片输入接口，但现有 SG3000 驱动的实时回调和 DMA 交接仍需板端接入。

## 主机调用和硬件接入

离线：`NativeEngine` 运行与 IP 相同的 C++，`examples/run_linescan.py` 串起碎片输入、校正/分块、合并、PGM/JSONL 输出；支持已有 P5 Mono8 图像。Windows 需要 Python、NumPy；测试另需 pytest。

真实硬件：使用 `mmio_runtime.TileExecutor`，实现 backend 的 `memory_words`、`dma_address`、read32、write32、sync_for_device、sync_for_cpu、reset_and_drain。同步函数必须包含缓存一致性和 MMIO 顺序保证。后端独占核和所有在途缓冲区。将 arena 内容复制到真实映射的 DMA 分配区，再把完成后的输出复制回；NativeEngine 的 ndarray 虚拟地址不能直接写入寄存器。

控制映射：00 control，10/14 DDR 地址，1c memory_words，24 input_offset，2c stride_bytes，34 image_width，3c image_height，44 core_x，4c core_y，54 core_width，5c core_height，64 radius，6c calibration_offset，74 output_offset，7c polarity，84 threshold，8c flags，94 return_status。均为十六进制字节地址。已逐项对照 HLS 生成的驱动头。

超时/启动不确定时隔离该分配区，不能释放或复用输入、校正、输出。仅当后端确认 master 停止且在途写已排空后恢复；晚到的 done、idle 或清 ap_start 都不能代替 reset/drain。PCIe SG3000 的设备 IOVA 不一定是 PL master 的 DMA 地址，不能直接照抄。

Vivado：导入随包 IP repository；PS AXI master 接控制口，经互连将本核 DDR master 接可访问 DDR 的 PS slave 口，连接时钟/复位并分配控制地址。控制基址、时钟、缓存一致性、设备树/驱动均取实际 Block Design/XSA；本包没有虚构地址，也没有可直接覆盖 SD 的 BOOT.BIN。

## 重现验证

`verify.ps1`：编译硬件 C++ 参考与 DLL，运行 329 个合法、34 个非法 native 案例、C++ CC 的 3000 帧随机洪泛对照，以及 Python 条带/缓存/背压/拼接/MMIO/PGM 测试。所有测试必须实际执行，无 skipped。

`build-hls.ps1 -Mode cosim`：Vitis HLS 2022.1，xczu9eg-ffvb1156-2-e，10 ns。实际 C/RTL 协同仿真 9 次连续调用，包含 16K 边缘、非对齐核心、最大 256×64/r7、半径切换、错误调用和写保护。`export-ip.ps1` 只导出源码哈希与通过协同仿真的工程一致的 IP。

当前综合：18 BRAM18K、13 DSP、6820 FF、10481 LUT，估计周期 7.300 ns；读缓存、校正缓存、窗口计算、输出存储循环均达到 II=1。各阶段串行运行，不能据此宣称整个流水线达到每周期一个像素。9 次 cosim 最大延迟 71273 周期；DDR 仿真模型、实际仲裁和处理器调度不同，这不是相机整线吞吐保证。未做布局布线时序收敛。

详细证据放在 `evidence/`，导出 IP 和 DLL 放在 `prebuilt/`。AArch64 测试 ELF 若在包内，仅表示静态交叉编译成功；没有执行它的板端验证。
