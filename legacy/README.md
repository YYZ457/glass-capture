# legacy · 原始代码，只读

`sg3000-core-v0.2/` 是 `玻璃检测_FPGA模块包_20261024/SG3000Workbench/core/` 的原样拷贝（外加 `build/receiver.ps1`）。

- **不要改这里的文件。** 新代码写进 `board/` 和 `include/` 的对应位置，每个骨架文件头部都写着"迁移来源：第几行到第几行"。
- 在一个模块迁移完、并且验证"新版和旧版输出逐字节相同"之前，板上继续用这里编译出的程序。
- 全部迁移完成后可以删除这个目录（git 历史里还在）。

电脑上编译检查（不需要板子）：

```
gcc -std=c11 -O2 -Wall -Wextra -Werror sg3000_capture.c sg3000_capture_core.c -o sg3000-capture
```

交叉编译给板子：把 `gcc` 换成 `aarch64-linux-gnu-gcc` 并加 `-static`，或用 `receiver.ps1`（Windows + Vitis 自带编译器）。
