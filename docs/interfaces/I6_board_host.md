# I6 · 板子 → 电脑

## 现在（legacy）

- 控制：电脑经 USB 串口（115200）往板上 shell 敲命令（`host/console/transport.py`）
- 图像：板上 gzip → hexdump 文本 → 串口回传，一帧压缩后约 1.7 MB，约 7 分钟
- 连续采集结果：板上程序每行输出 `SGJSON {...}`（`board/transport/tx_jsonl.c`）

## 计划（⬜ 草案，写 tx_tcp / rx_tcp 时定稿）

TCP，板子主动连电脑，默认端口 8770（待定）。每条消息 = 固定 32 字节头 + 负载，小端：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u32 | 魔数 `"GCAP"` |
| 4 | u16 | 版本 = 1 |
| 6 | u16 | 类型：1 原图块 / 2 候选列表 / 3 状态 |
| 8 | u64 | 消息序号 |
| 16 | u32 | block_id |
| 20 | u32 | 负载字节数 |
| 24 | u32 | 负载 CRC32 |
| 28 | u32 | 保留 |

- 类型 1 负载：u32 width、u32 height、再接 width×height 字节 Mono8
- 类型 2 负载：u32 count，再接 count 个 `struct sg_candidate`（`board/processing/sg_candidates.h`）
- 类型 3 负载：一行 JSON

电脑端存下来的候选格式对齐 `schemas/frame-candidates-v1.schema.json`。

## 带宽

满速原图 ≈ 252 MB/s，千兆网 ≈ 110 MB/s：类型 1 只用于调试和抽样。见 D002。
