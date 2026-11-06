# I5 · 采集协议层对外接口

上层（`board/apps/`、`board/stream/`）只调用下面这些函数，不碰寄存器。

## 向下：回调表 `include/sg_capture_io.h`

`read_reg`、`write_reg`、`dma_read`、`now_ms`、`sleep_ms`、`cancelled`。
板上由 `board/capture/sg_io_linux.c` 实现；电脑上测试时可以写一个假的实现。

## 向上：函数

| 函数 | 文件 | 作用 |
|---|---|---|
| `sg_capture_default_options` / `sg_capture_session_init` | sg_session | 准备会话 |
| `sg_capture_preflight` | sg_preflight | 只读体检 |
| `sg_stream_begin` / `next` / `end` | sg_acq | 连续采集 |
| `sg_capture_once` | sg_acq | 单帧 = begin + next + end |
| `sg_capture_frame_free` | sg_session | 释放块 |

## 内部（capture/ 之外不要用）

`sg_mbx_read` / `sg_mbx_write`（sg_mailbox）、`sg_block_header_read` / `check` / `receive`（sg_block）、`sg_session_rd` / `wr` / `cancelled`。

## 文件内依赖

```
sg_acq ──▶ sg_preflight ──▶ sg_mailbox ──▶ sg_session ──▶ sg_capture_io（回调表）
   └─────▶ sg_block ─────────────────────▶ sg_session
```
