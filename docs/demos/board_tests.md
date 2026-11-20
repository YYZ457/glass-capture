# board/tests 走读：电脑上能证明什么，不能证明什么

*2026-11-20T14:14:53Z by Showboat 0.6.1*
<!-- showboat-id: 7c24481f-b92d-4ba6-b057-8a04f200e237 -->

这是一份用 [Showboat](https://simonwillison.net/2026/Feb/10/showboat-and-rodney/) 生成的**可执行**演示文档：每个代码块都真实执行过，下面的输出是 Showboat 捕获的，不是手写的。复验方法（在仓库根目录）：

```bash
uvx showboat verify docs/demos/board_tests.md --workdir board
```

它会重跑所有命令并逐块对比输出；代码一改、行为一变，这里就会报 diff。

目标读者：想弄明白"板上程序在电脑上被测到了什么程度"的人。三组测试都**不需要板子、相机和 FPGA**，原理是把所有硬件访问都收拢到一张回调表（`struct sg_io`：读寄存器、写寄存器、DMA、睡眠、取时间），测试时塞进一张假的。

## 1. 三个测试程序，各管一层

```bash
wc -l tests/*.c
```

```output
   43 tests/driver_whitelist_test.c
  377 tests/fake_card_test.c
  622 tests/unit_test.c
 1042 total
```

- `driver_whitelist_test.c`：内核驱动 `sg3000_rx.ko` 只允许用户态读写少数寄存器、写少数值。驱动本身要板子的内核源码才能编，但白名单这段是纯函数，Makefile 用 `sed` 把它从驱动源码里抠出来单独编译测试。
- `fake_card_test.c`：一张"假 SG3000 采集卡"，模拟邮箱协议、块头、DMA 和停流行为，测 `board/capture`（会话、邮箱、预检、取块、开始/停止）和 PGM 写盘。
- `unit_test.c`：采集之后的东西——图像视图/统计、候选、行检查、双线程流水线、相机串口、相机参数设置、PGM 检查和噪声/轮廓统计。

## 2. 全部跑一遍

`peak=` 是流水线队列的峰值深度，取决于两个线程谁跑得快，每次可能是 1 或 2（实测 10 次里 6 次是 1、4 次是 2）。为了让 `verify` 可复现，这里把它规整成 `peak=N`。这本身也是一条值得知道的信息：**这个数字不能当回归指标用**。

```bash
make -s clean && make -s test 2>&1 | sed "s/peak=[0-9]*/peak=N/"
```

```output
fake card: ALL PASS
driver whitelist ok
view/stats ok
candidates ok
linecheck ok
pipeline normal: ret received=300 processed=300 peak=N qfull=0 stops=1
pipeline bad: ret=-105 proc_err=0 src_err=-105 stops=1
pipeline ok
cam_serial ok
cardxml url ok
cam manifest ok
cam set ok
pgmcheck ok
stats row ok
screen ok
pgm noise ok
pgm profile ok
unit: ALL PASS
```

## 3. 驱动白名单：为什么内核驱动要限制寄存器

驱动通过 ioctl 把 BAR0 寄存器读写开放给用户态程序。写错一个寄存器，轻则卡状态错乱，重则 PCIe 报错整板重启——所以驱动只放行明确需要的地址和值。关键规则：

- 写：只允许 4 个地址（邮箱发送触发、ARM、ACK 等），且值也受限（例如 `0x201000` 只能写 2）。
- 相机串口（`cam_serial=1` 加载参数）默认关闭；打开后也**只开放端口 0**。
- 端口 1 一律不放开：2026-11-14 实板上，单路卡碰端口 1 的地址会触发 nwl-pcie Slave error，整板要重启。测试里专门有一行断言守住这条：

```bash
grep -n -B1 "0x413004" tests/driver_whitelist_test.c
```

```output
36-    /* 端口 1 一律不放开：单路卡上碰它会 PCIe Slave error（2026-11-14 实板）*/
37:    assert(!sg_reg_allowed(0x413004, false) && !sg_reg_allowed(0x413010, true) && !sg_reg_allowed(0x41301c, false));
```

## 4. 假卡：14 个场景就是一份"采集卡会怎么坑你"的清单

`fake_card_test.c` 的每个编号场景对应一种真实或预想的卡行为。直接把场景标题抽出来，基本就是 capture 层的需求清单：

```bash
grep -oE '/\* [0-9]+[a-z]?\. [^*]*' tests/fake_card_test.c | sed 's#^/\* ##'
```

```output
1. 会话初始化：缺回调 → -EINVAL 
2. 邮箱：连读两次 DDR，值=2，ACK 连续 
3. 预检 
3b. 读卡存储（sg_cardxml）：URL 字节顺序、解析、按地址读出文件 
3c. 写卡参数：白名单、值域、写入格式（第 7 字 = 值）、读回 
4. 块头检查（纯函数） 
5. 单帧：begin+next+end，2 包拼成 16x8 
6. 块头魔数错 → EPROTO，仍执行 STOP，且帧已释放 
7. 开始前已有块就绪 → EBUSY，且不尝试 START 
8. 超时：块永远不就绪 
9. 连续两块，块号 7、8 连续；再制造不连续 
10. 容量上限 
11. 高带宽停流：STOP 后等 2.5 秒才转成 "STOP"（超过 control_timeout_ms=1000，低于 6000）→ 仍成功 
12. 真正卡死：STOP 后 7 秒仍未停 → 报 -ETIMEDOUT 且定位到 wait SG_ACQ_STATE = STOP 
13. STOP 后 FIFO 里仍有 7 个未取块头，读 SG_HDR_READY 逐个弹出后立即转成 "STOP" 
14. SG_HDR_READY 永久卡在非 0 时也不能死循环，仍受 deadline 限制报 -ETIMEDOUT 
```

其中 11–14 是同一个故事，来自 2026-11-14 在 ZCU102 上的实板问题（提交 `e6b7ea7`）：高带宽（16384×2000 @ 10 kHz）停流时，发出 STOP 后卡的状态一直不变成 "STOP"。原因写在 `stop_capture` 的注释里：

```bash
sed -n '/^\/\* 发 STOP/,/^#define SG_STOP_WAIT_MIN_MS/p' capture/sg_acq.c
```

```output
/* 发 STOP 命令，排空停采瞬间仍已进入 FIFO 的待读块头，然后等 SG_ACQ_STATE 变成 "STOP"。
 * 邮箱写用 cleanup = 1 与 control_timeout_ms（1..2 秒）：用户按了 Ctrl+C 也要把 STOP 发完。
 * 2026-11-14 ZCU102 实测 16384×2000 @ 10 kHz：stop 发出时未 ACK 的块仍留在 SG_HDR_READY
 * （0x20206c）FIFO 里，且该 FIFO 为"读即弹出"；必须先读到 SG_HDR_READY == 0，FPGA 才会在约
 * 10 ms 后把 SG_ACQ_STATE 转成 "STOP"（否则死等也不转）。保留 SG_STOP_WAIT_MIN_MS（6 秒）
 * 作为突发积压时的等待上限。 */
#define SG_STOP_WAIT_MIN_MS 6000u
```

也就是说：`SG_HDR_READY` 不是普通的状态寄存器，而是一个**读即弹出**的 FIFO。停流时 FIFO 里还积着没取走的块头，FPGA 要等它被读空才肯进入 STOP。所以等待循环里每一轮都要读一次 `SG_HDR_READY`——这一读看起来像"顺便查个状态"，实际上是在**排空 FIFO**。

这种"看起来可有可无、删掉就出事"的代码是理解债的重灾区。下面做个反向实验：在临时副本里把这一读换成 `ready = 0`（模拟"有人觉得它没用顺手删了"），看测试能不能抓住：

```bash
T=$(mktemp -d) && cp -r . "$T/board" && cp -r ../include "$T/" && rm -rf "$T/board/out" && sed -i "/^static int stop_capture/,/^}/s/ret = sg_session_rd(s, SG_HDR_READY, &ready);/ready = 0; ret = 0;/" "$T/board/capture/sg_acq.c" && (cd "$T/board" && make -s test 2>&1 | grep -E "FAIL|PASS"); rm -rf "$T"
```

```output
FAIL line 365: sg_capture_once(&s, &o, &r) == 0
FAIL line 366: C.stop_drain_hdrs == 0 && r.stop_error == 0 && r.frame.width == W && r.frame.height == MAXL
fake card: 2 FAILED
```

只有场景 13（第 365–366 行，"STOP 后 FIFO 里仍有 7 个未取块头"）失败，其余 13 个场景都没察觉。结论：

1. 这段修复是**被测试保护的**，误删会立刻在电脑上暴露，不用等上板。
2. 但保护它的只有这一个场景。改 `stop_capture` 时要知道场景 13 是它的"看门狗"。

## 5. 流水线：一个故意宽松的断言

`sg_pipeline` 的设计原则是"队列满不丢块，直接以 `-ENOBUFS` 结束"——处理跟不上就算系统不合格，必须让人知道。测试里故意喂一个总是返回 `-EIO` 的处理器，看流水线能不能干净地停下来：

```bash
grep -n -A3 'p.process = bad_process' tests/unit_test.c
```

```output
137:    p.process = bad_process; p.close = NULL; stops = 0;
138-    int ret = sg_pipeline_run(&s, &p, &k, &o, &st);
139-    printf("pipeline bad: ret=%d proc_err=%d src_err=%d stops=%d\n", ret, st.processing_error, st.source_error, stops);
140-    assert(st.processing_error == -EIO || st.source_error == -ENOBUFS);
```

注意断言接受**两种**结果：`-EIO`（处理线程先报错）或 `-ENOBUFS`（采集线程先发现队列满）。两个线程谁先撞上错误是竞争出来的，所以测试只要求"以其中一种错误结束、stop 恰好调用一次"。在这台机器上实测 30 次都是 `-ENOBUFS`，但换一台机器结果可能不同——这不是 bug，是有意为之；如果将来有人把断言收紧成只认一种，就会得到一个时好时坏的测试。

## 6. 这些测试证明不了什么

电脑上的测试只覆盖"逻辑按我们理解的协议走"。下面这些**只有上板才能验证**（见 `board/scripts/board_session.sh` 和 `docs/NEXT_STEPS.md`）：

- 内核驱动本身（DMA、中断、`dma_fast`）——这里只测了白名单片段。
- 假卡的行为是否真的等于真卡。假卡是按实板现象写的，真卡出现新行为时，假卡不会自己跟上。
- 吞吐量（目标 15 kHz × 16384 ≈ 246 MB/s）、处理线程在 ARM 上的实际速度。
- 相机、CameraLink 线缆、FPGA 比特流。

所以读这份文档的正确姿势是：**测试绿 = capture/processing 层的逻辑没退化**；**不等于板子能用**。

