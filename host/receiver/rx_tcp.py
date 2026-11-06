"""rx_tcp · 电脑端接收板子经网口发来的数据（I6 协议）。  ⬜

职责：监听端口 → 按 32 字节头拆消息 → 校验 CRC → 落盘
  类型 1 原图块 → runtime/frames/<block_id>.pgm
  类型 2 候选   → runtime/candidates/<block_id>.json（frame-candidates-v1 格式）
  类型 3 状态   → 追加到 runtime/status.jsonl
不做任何检测；中控台从落盘目录读。

对端：board/transport/tx_tcp.c
用法：python -I host/receiver/rx_tcp.py --port 8770 --out runtime/
"""
import sys

HEADER_SIZE = 32
MAGIC = b"GCAP"


def main() -> int:
    print("rx_tcp: not implemented yet", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
