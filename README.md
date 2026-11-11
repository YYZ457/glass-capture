# glass-capture

透明玻璃表面微颗粒线扫检测 · 采集与处理代码。

相机 JAI SW-WS2-CL16K-50M-SDE（凌云光型号 SF6200-CL16K-50M）→ 凌云光 SG3000 采集卡 → ZCU102（ARM Linux）→ 电脑。

厂家资料与核对结论：`docs/vendor/README.md`。

## 这个仓库是什么状态

**代码已写全，等上板。** 除了相机串口（`cam_serial`，缺硬件信息）和 PL 处理器（D001 决定不做），所有模块都有实现；
电脑上有假卡测试、单元测试、模拟板子的中控台端到端测试。还没有一个新程序在真板子上跑过。

**要上板：看 `docs/BRINGUP.md`。** 板上程序已编译好放在 `dist/glass-capture/`，拷到 SD 卡即可。

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
python host/console/launch.py                        # 启动中控台（Windows）
make -C board check                                  # 电脑上检查所有接口能编译
make -C board test                                   # 假卡测试 + 单元测试
python tools/make_sd_bundle.py                       # 交叉编译并生成 dist/glass-capture（需要 aarch64-linux-gnu-gcc）
board/driver/module.sh                               # 构建驱动（需要对应内核源码）
python -m pytest host/tests -q                       # 中控台测试
```

`host/tests` 里离线检测演示（defects / frames）的测试需要原始 zip 包里的 `sample_output_16k/` 和预编译 DLL，仓库里没有，会报错；其余应全部通过。

## 不在这里的东西

CAD、清洗箱、物料清单、实机日志、采集到的图像、HLS 生成的 IP 和预编译二进制。
原始完整包：`玻璃检测_FPGA模块包_20261024.zip`。
