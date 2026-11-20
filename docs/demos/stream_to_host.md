# 连续采集全链路：从 sg3000-stream 到电脑上的文件

*2026-11-20T14:26:51Z by Showboat 0.6.1*
<!-- showboat-id: 46c2ca7e-0fd3-4fd3-b303-a341db7179f0 -->

用 [Showboat](https://simonwillison.net/2026/Feb/10/showboat-and-rodney/) 生成的可执行文档，输出均为真实执行捕获。复验（仓库根目录）：

    uvx showboat verify docs/demos/stream_to_host.md --workdir board

上一份 `capture_protocol.md` 讲的是"一个块怎么从卡上到 ARM 内存"。这一份接着往下走：**块进了内存之后，怎么被处理、怎么发到电脑上、电脑上落成什么文件。** 用的是 `sg3000-stream --source synthetic`（合成数据源，不需要采集卡），所以整条链路在任何 Linux 电脑上都能跑；唯一被替换掉的是最前面的"数据源"。

## 1. 程序是怎么拼起来的

`board/apps/sg_stream_main.c` 本身几乎没有逻辑，只做组装。四个零件都是"一张回调表"，互相不认识：

    数据源 sg_source        硬件（sg_acq 的 begin/next/end）或 合成（--source synthetic）
       ↓ 一个块
    引擎   sg_pipeline_run  主线程取块 → 有界队列 → 处理线程
       ↓
    处理器 sg_processor     sg_stats 或 sg_candidates（--processor candidates）
       ↓ 一个结果
    出口   sg_result_sink   tx_jsonl（stdout 上的 SGJSON 行，经串口给中控台）
                            加 --tcp 时前面再串一个 tx_tcp（网口，I6 协议）

这就是 ARCHITECTURE.md 里"依赖只能从上往下"的落地方式：`stream/` 不知道数据从哪来、怎么处理、往哪发。

```bash
make -s out/sg3000-stream && out/sg3000-stream --help 2>&1 | head -8
```

```output
sg3000-stream 0.2 / interface ABI 1
--source hardware|synthetic --processor stats|candidates --seconds 30 (0=until stopped)
--max-blocks 0 --queue 4 --max-frame-mib 16 --block-policy observe|strict
--process-timeout-ms 2000 --emit-ms 1000 --threshold 128 (candidates: 25) --device PATH
Candidates: --min-area 2 --max-area 0 --max-candidates 4096 --background FILE.csv
Network: --tcp IP[:PORT] (send candidates per block) --tcp-frame-every 0 (send raw block every N)
Synthetic only: --width 256 --height 64 --synthetic-delay-ms 50
Output: SGJSON lines to stdout; no automatic image/result files or external actuation.
```

## 2. 跑一次：5 个块，每 2 块附发一次原图

电脑这一端先用 `docs/demos/src/peek_i6.py` 监听。它复用 `host/receiver/rx_tcp.py` 里真正的解析函数，只是把每条消息头都打印出来。板子那一端跑 5 个合成块，处理器选 `candidates`，`--tcp-frame-every 2` 表示每 2 块额外发一次原图。

为了能复验，时间戳、耗时和队列峰值这几个每次都不同的字段换成了 `…`；状态消息的长度会随这些数字的位数变化，也一并规整掉。

```bash
(python3 -I ../docs/demos/src/peek_i6.py 18771 > out/peek.txt 2>&1 &) ; sleep 0.5
echo "== 板子 stdout（SGJSON，走串口给中控台）=="
out/sg3000-stream --source synthetic --processor candidates --max-blocks 5 --synthetic-delay-ms 10 --tcp 127.0.0.1:18771 --tcp-frame-every 2 | sed -e 's/"received_ns":"[0-9]*"/"received_ns":"…"/; s/"elapsed_ms":"[0-9]*"/"elapsed_ms":"…"/; s/"queue_peak":[0-9]*/"queue_peak":…/'
sleep 0.5
echo; echo "== 电脑收到的网口消息（I6）=="
sed -E "s/状态    block 0  负载 +[0-9]+ 字节/状态    block 0  负载 （长度不定）/" out/peek.txt
```

```output
== 板子 stdout（SGJSON，走串口给中控台）==
SGJSON {"schema":1,"event":"started","source":"synthetic","processor":"candidates","queue_depth":4,"max_frame_bytes":"16777216","continuity_verified":false}
SGJSON {"schema":1,"event":"result","source":"synthetic","processor":"candidates","sequence":"1","received_line_start":"0","received_ns":"…","block_id":1,"width":256,"height":64,"stride":256,"flags":2,"processed_total":"1","decision":"unknown","algorithm":2,"pixel_count":"16384","sum":"2088960","above_threshold":"0","min":0,"max":255,"continuity_verified":false}
SGJSON {"schema":1,"event":"ended","source":"synthetic","processor":"candidates","error":0,"source_error":0,"processing_error":0,"output_error":0,"stop_error":0,"cancelled":false,"received":"5","processed":"5","bytes":"81920","id_discontinuities":"0","queue_full_events":"0","queue_peak":…,"elapsed_ms":"…","dma_chunk":0,"dma_bytes":"0","dma_ms":"0","wait_ms":"0"}

== 电脑收到的网口消息（I6）==
第 1 条消息头原始字节：
  47 43 41 50 01 00 02 00 01 00 00 00 00 00 00 00
  01 00 00 00 08 00 00 00 69 df 22 65 00 00 00 00
  = 魔数 'GCAP' | 版本 u16 | 类型 u16 | 序号 u64 | block_id u32 | 负载长度 u32 | CRC32 u32 | 保留 u32（小端）
序号  1  候选    block 1  负载      8 字节  CRC 对  0 个候选
序号  2  候选    block 2  负载      8 字节  CRC 对  0 个候选
序号  3  原图块   block 2  负载  16392 字节  CRC 对  256x64
序号  4  候选    block 3  负载      8 字节  CRC 对  0 个候选
序号  5  候选    block 4  负载      8 字节  CRC 对  0 个候选
序号  6  原图块   block 4  负载  16392 字节  CRC 对  256x64
序号  7  候选    block 5  负载      8 字节  CRC 对  0 个候选
序号  8  状态    block 0  负载 （长度不定）  CRC 对  
连接正常关闭
```

读这段输出要注意的几件事：

- **两个出口同时在工作。** stdout 上的 SGJSON 行是给串口和中控台看的"仪表盘"，按 `--emit-ms`（默认 1 秒）节流，所以 5 个块只打出 1 行 `result`。网口上则**每块都发**候选结果，这是给电脑做后续处理的数据通道。
- **I6 消息头固定 32 字节、小端**：`47 43 41 50` 就是 ASCII "GCAP"，类型 2 是候选，负载长度 8 字节就是"候选数 u32 + 标志 u32"、后面跟 0 条记录。每条消息都带 CRC32，电脑端校验失败就**直接断开连接**，不尝试跳过坏消息（`rx_tcp.py` 开头的规则）。
- **原图消息 = 8 字节宽高 + 像素。** 256×64 = 16384，加 8 正好是 16392。
- **最后一条是状态消息**（block 0），内容就是 stdout 上那行 `ended` 的 JSON。
- **为什么 0 个候选？** `sg_candidates.h` 写明：没有标定背景（`--background`）时，用这一块自己的**逐列均值**当背景。合成图的像素是 `(i + 块号) & 0xff`，宽度 256，所以每一列都是常数，减掉列均值后处处为 0。这也说明了检测器的基本假设：线扫相机下，同一列的背景应该是稳定的。

## 3. 真正的接收端落盘成什么

换成真正的接收端 `host/receiver/rx_tcp.py`（中控台里内置的也是同一份代码），看它把文件放在哪里：

```bash
sh ../docs/demos/src/rx_demo.sh
```

```output
./candidates/0000000001_1.json
./candidates/0000000002_2.json
./candidates/0000000004_3.json
./candidates/0000000005_4.json
./candidates/0000000007_5.json
./frames/0000000003_2.pgm
./frames/0000000006_4.pgm
./status.jsonl

== candidates/0000000002_2.json ==
{
 "schema_version": "board-candidates-v0",
 "sequence": 2,
 "block_id": 2,
 "bbox_convention": "half-open",
 "connectivity": 8,
 "decision": "unknown",
 "truncated": false,
 "count": 0,
 "candidates": []
}
== frames/0000000003_2.pgm 的文件头 ==
   P   5  \n   2   5   6       6   4  \n   2   5   5  \n 002
```

文件名是 `<消息序号>_<块号>`：序号是**消息**的序号（候选和原图共用一个计数），不是块号，所以 `0000000003_2.pgm` 是第 3 条消息、内容是第 2 块的原图。写文件时先写 `.partial` 再改名，目标已存在就报错，绝不覆盖（`write_new`）。

## 4. 处理跟不上时：直接失败，不丢块

`sg_pipeline.c` 开头写着规则："队列满不丢块：直接以 `-ENOBUFS` 结束（处理跟不上 = 系统不合格，必须让人知道）"。让合成源不停顿地喂 4096×2000 的大块（和实板一样大）：

```bash
out/sg3000-stream --source synthetic --processor candidates --max-blocks 200 --synthetic-delay-ms 0 --width 4096 --height 2000 2>/dev/null | grep ended | grep -oE "\"(error|source_error|processing_error|queue_full_events)\":\"?-?[0-9]+\"?|\"queue_peak\":[0-9]+"
```

```output
"error":-105
"source_error":-105
"processing_error":0
"queue_full_events":"1"
"queue_peak":4
```

`-105` 就是 `-ENOBUFS`，而且是**数据源一侧**报的（`source_error`）：采集线程要往队列里放块时发现队列满了。`queue_peak = 4` 正好等于默认队列深度 `--queue 4`。（这里只摘了稳定的字段；`received` 和 `processed` 每次差一两块，取决于线程调度。）

为什么不丢块继续跑？因为这个系统要做在线检测，丢一块就是一段玻璃没被看过，宁可停下来让人知道。代价是：**板上处理速度必须稳定地快于相机出块速度**，这正是 `sg3000-capture bench` 和 `docs/design/detection_plan.md` 里算力估算要回答的问题。

## 5. 这条链路上还没被证明的东西

- **`continuity_verified` 永远是 `false`。** 它在 `sg_stream_main.c` 和 `tx_jsonl.c` 里是写死的：块号连续只说明没丢"块"，不说明没丢"行"；真正检查累计行数和行频的 `stream/sg_linecheck.c` 写好了、有单测，但还没接进程序（ARCHITECTURE.md 也这么写）。
- **硬件源。** 这里的数据源是合成的，替换成硬件源后，上一份文档里的协议才会真正出场。`sg_stream_main.c` 文件头写"硬件源从没在实机上跑过"，但 ARCHITECTURE.md 记录 `--source hardware --processor stats` 已实机通过——**两处状态不一致，以实板日志为准，建议更新其中一处**。
- **候选检测本身的正确性。** 这里只证明了"候选结果能完整送到电脑"，检测算法有没有找对颗粒是另一个问题（`verify/V4` 真值比对，状态 🟡）。

