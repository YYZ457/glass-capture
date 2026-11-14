# 给 AI 助手的项目规则

玻璃微颗粒线扫检测：相机 SF6200-CL16K → SG3000 采集卡（PCIe）→ ZCU102（aarch64 Linux 5.15.19-xilinx-v2022.1）→ 电脑。
负责人是工科学生，C 基础一般：解释要简短，用中文；改动要小、可验证。

## 先读

0. `docs/NEXT_STEPS.md` —— 现状、下次上板做什么、协作规则（谁推 `main`、谁只提交 `.ko`）
1. `docs/OVERVIEW.md` —— 哪些是黑箱、哪些还在攻关
2. `ARCHITECTURE.md` —— 每个文件的职责和状态（✅ 实机证明 / 🟡 写了没上板 / ⬜ 空 / ❓ 缺资料）
3. `docs/BRINGUP.md` —— 上板操作步骤
4. `docs/decisions/` —— 已做的决定，不要推翻：D001 FPGA 不进数据通路；D002 原图出不了板子，只发候选；D003 不给二进制打补丁

## 硬规则

- **不要猜寄存器，不要试写寄存器。** 所有地址在 `include/sg3000_regs.h`，每条标了可信度（[逆向] = 反汇编得来、没在 Linux 跑过；新的逆向结论记到 `docs/vendor/reverse_engineering_*.md`，写明反汇编地址）。驱动新加的相机串口白名单默认关闭（`cam_serial=1` 才放开），不要改成默认打开。Windows 原始二进制在 `docs/vendor/windows_bin/`，新结论要写反汇编地址。写错卡的寄存器可能让卡进入异常状态。驱动和邮箱各有写白名单，不要放宽。
- **不要修改二进制文件**（D003）。改了代码就重新编译：`python tools/make_sd_bundle.py`。
- **不要静默丢数据。** 队列满、候选截断、块号跳变都必须报出来（错误码或标志），这是整个项目的一贯做法。
- **错误约定**：成功返回 0，失败返回负 errno（含义见 `include/sg_err.h`）。
- **板上程序的输出文字不要改**：`host/console/transport.py` 和 `telemetry.py` 用正则解析它们（`DDRInitialStatus=`、`Saved complete Mono8 frame:`、`SGJSON {...}` 等）。
- **串口命令必须是纯 ASCII**（`transport.py` 里发给板子的 shell 命令不能有中文）。
- **`legacy/` 只读**，是迁移参照。
- **卡参数写白名单**在 `sg_mbx_write_allowed()`（`sg_mailbox.c`）和中控台 `CARD_SET_RANGES`，两处必须一致。加新可写参数前先在 `docs/vendor/card_xml/` 的 XML 里查值域，并且只加需要的那一个。XML 里的 `DeviceReboot`、`ManualFPGAReload` 这类寄存器永远不能加。
- **卡邮箱有两种读**：寄存器用 `sg_mbx_read`（严格检查，实测可靠）；卡存储（URL / XML）用 `sg_mbx_read_block`（照 VisionKit 只查两项）。不要把严格检查放宽到寄存器读上。
- **驱动白名单改了要跑 `make -C board test`**（含 `driver_whitelist_test`）。
- **检测算法 C 和 Python 两份必须一致**：`board/processing/sg_candidates.c` ↔ `host/detect/candidates.py`。改一边就改另一边。
- 板上程序静态链接，不要依赖动态库或域名解析。

## 改完必须跑

```
make -C board check
make -C board test
python -m pytest -q host/tests/test_console.py host/tests/test_board_sim.py host/tests/test_candidates_ref.py host/tests/test_audit_transport.py
```

有 ARM64 交叉编译器和 qemu 时再跑：`make -C board CROSS_COMPILE=aarch64-linux-gnu- RUN=qemu-aarch64-static test`。
`host/tests` 里 defects / frames / offline_frames 的测试需要仓库外的示例数据和 DLL，报错是已知情况。

改了板上代码：重新生成 `dist/glass-capture/`，更新 `ARCHITECTURE.md` 状态列。
