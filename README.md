# glass-capture

透明玻璃表面微颗粒线扫检测 · 采集与处理代码。

相机 SF6200-CL16K → SG3000 采集卡 → ZCU102（ARM Linux）→ 电脑。

## 这个仓库是什么状态

**骨架。** 结构、接口、每个文件的职责已经定好；大部分 `.c` 文件里只有函数签名和 `return -ENOSYS`，等待一个个填入确定的代码。
现在能在板上跑的程序仍然来自 `legacy/`。

## 从哪读起

1. `ARCHITECTURE.md`：全链路、依赖方向、每个文件的职责和状态、填写顺序
2. `docs/interfaces/`：层与层之间的约定。**写代码前先改这里，代码跟着约定走**
3. `include/sg3000_regs.h`：所有寄存器地址，每条标着是厂家给的、观察出来的还是猜的
4. `docs/decisions/`：为什么这样做

## 每个骨架文件的头部写着

- **职责**：只做什么
- **依赖 / 被谁用**
- **迁移来源**：legacy 里第几行到第几行
- **怎么验证**：做完怎么证明它是对的

## 常用命令

```
make -C board check                                  # 电脑上检查所有接口能编译
make -C board CROSS_COMPILE=aarch64-linux-gnu-       # 交叉编译板上程序
board/driver/module.sh                               # 构建驱动（需要对应内核源码）
python -m pytest host/tests -q                       # 中控台测试
```

## 不在这里的东西

CAD、清洗箱、物料清单、实机日志、采集到的图像、HLS 生成的 IP 和预编译二进制。
原始完整包：`玻璃检测_FPGA模块包_20261024.zip`。
