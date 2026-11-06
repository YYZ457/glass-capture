# 后续拼装约束

先读 README.md 与 contract.json。以已有测试和生成驱动头为准，不重新猜时序、寄存器、符号位或 halo。

- 本模块输入 packed Mono8，v1 AI 输入每元素一个 uint32；必须显式转换，输出 signed9 也不能按 int8 直接截断。
- 完整条带 halo → 核心 tile → 完整核心行 → CC 的顺序固定。保留全局坐标、frame_id、坏列有效位。拼接只写核心一次。
- 校正在整帧固定。不能在 tile 或 stripe 边界重置 CC，不能把内部边界当图像边缘。
- 接硬件时只新增平台 backend。复用 TileExecutor 的单次启动、缓存同步、超时隔离与 reset/drain 合同；不硬编码未知物理地址，不复用未证明兼容的 PCIe IOVA。
- C++ CC 是 CPU 模块；Python native 演示不是 FPGA 上板。综合 II/估计时序不是真实吞吐或收敛时序。亮场历史样本不能证明暗场精度。
- 改动 HLS 必须 native 对照、综合、RTL cosim、接口核对后重新导出，更新证据/哈希；不能搭配旧 IP。改条带或 CC 必须保留整图 oracle、16K、跨 x256/y64、碎片/尾行/背压测试。
- 不改冻结 v1。不要修改用户 SD、相机、采集卡或板端启动文件来运行离线示例。
