# 接续本模块

本目录是用户要求保留的可组合基础模块。优先写adapter/config/model graph，不随意重写RTL、量化算术或启动/停止握手。

- 先读README、contract.json、validation.json，明确CPU测试/HLS综合/RTL协同仿真/实板的不同证据层级。
- 同时适用上级AGENTS；不要修改sources/或D盘旧算法工程。legacy/内保留代码仅作冻结参考。
- 数据offset用32位word；MMIO offset用byte；CHW/OIHW；负数ties-away舍入。不能默默改变约定。
- 修改算法/数值必须新增独立预期值测试，重跑verify.ps1；修改HLS必须重跑综合和小规模RTL cosim，确认寄存器表一致。
- 不支持的模型算子/量化布局显式拒绝。不要把示例权重当训练模型，不要把候选响应当概率，不要把非对称量化直接塞入对称INT8核。
- 不把Python地址、PCIe设备IOVA或旧7020物理地址直接传给ZCU102 PL。新后端必须实现并验证DMA分配、缓存同步和reset-and-drain。
- timeout/abort后不得释放或复用缓冲区，直到停止并排空得到确认。禁止把idle单独当排空证据。
- FIFO/framer/supervisor都不是CDC模块。跨时钟与复位域是单独任务。
- HLS2022.1 Windows工具已遇到特定嵌套池化循环编译器崩溃。若重构，必须真正运行综合，不以C++编译替代。
- 只接小规模示例成功不能说明16K线扫实时。保留性能/准确率/相机实测边界。
- 不连接硬件、不下载bitstream、不改SD/启动配置，除非当前用户任务授权需要该操作。
