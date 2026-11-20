"""peek_i6 · 演示用：在一个端口上收一条板子连接，逐条打印 I6 消息头。
复用 host/receiver/rx_tcp.py 里真正的解析函数（parse_header / recv_exact），只多做打印。
第一条消息额外打印 32 字节头的原始十六进制。只给 docs/demos/ 用。
用法：python3 -I docs/demos/src/peek_i6.py PORT
"""
import socket
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "host" / "receiver"))
import rx_tcp  # noqa: E402

NAMES = {1: "原图块", 2: "候选", 3: "状态"}
port = int(sys.argv[1])
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
srv.settimeout(10)
conn, _ = srv.accept()
first = True
while True:
    raw = rx_tcp.recv_exact(conn, rx_tcp.HEADER_SIZE)
    if raw is None:
        print("连接正常关闭")
        break
    if first:
        print("第 1 条消息头原始字节：")
        for i in range(0, 32, 16):
            print("  " + " ".join(f"{b:02x}" for b in raw[i:i + 16]))
        print("  = 魔数 'GCAP' | 版本 u16 | 类型 u16 | 序号 u64 | block_id u32 | 负载长度 u32 | CRC32 u32 | 保留 u32（小端）")
        first = False
    kind, seq, block_id, length, crc = rx_tcp.parse_header(raw)
    payload = rx_tcp.recv_exact(conn, length) or b""
    ok = "CRC 对" if zlib.crc32(payload) == crc else "CRC 错"
    extra = ""
    if kind == 1:
        w, h, _ = rx_tcp.decode_frame(payload)
        extra = f"{w}x{h}"
    elif kind == 2:
        items, truncated = rx_tcp.decode_candidates(payload)
        extra = f"{len(items)} 个候选"
    print(f"序号 {seq:2d}  {NAMES[kind]:4s}  block {block_id}  负载 {length:6d} 字节  {ok}  {extra}")
