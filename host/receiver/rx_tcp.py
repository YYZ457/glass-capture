"""rx_tcp · 电脑端接收板子经网口发来的数据（I6 协议）。

职责：监听端口 → 按 32 字节头拆消息 → 校验 → 落盘
  类型 1 原图块 → OUT/frames/<序号>_<block_id>.pgm
  类型 2 候选   → OUT/candidates/<序号>_<block_id>.json
  类型 3 状态   → 追加到 OUT/status.jsonl
不做任何检测。一次只接一个板子连接；连接断了就等下一个。
任何校验失败（魔数、版本、类型、长度、CRC）都断开这条连接，不尝试"跳过坏消息"。

对端：board/transport/tx_tcp.c
用法：python -I host/receiver/rx_tcp.py --port 8770 --out runtime/
"""
import argparse
import json
import os
import socket
import struct
import sys
import zlib
from pathlib import Path

HEADER_SIZE = 32
HEADER = struct.Struct("<4sHHQIIII")   # 魔数 版本 类型 序号 block_id 负载长度 CRC 保留
MAGIC = b"GCAP"
VERSION = 1
TYPE_FRAME, TYPE_CANDIDATES, TYPE_STATUS = 1, 2, 3
CANDIDATE = struct.Struct("<6I2f")     # x0 y0 x1 y1 area peak cx cy，32 字节
MAX_PAYLOAD = 64 * 1024 * 1024
MAX_STATUS = 64 * 1024


class ProtocolError(Exception):
    pass


def recv_exact(conn, n):
    """收满 n 字节；对方在消息边界正常断开返回 None，消息中途断开算协议错误。"""
    chunks, got = [], 0
    while got < n:
        part = conn.recv(min(n - got, 1 << 20))
        if not part:
            if got == 0:
                return None
            raise ProtocolError(f"connection closed mid-message ({got}/{n} bytes)")
        chunks.append(part)
        got += len(part)
    return b"".join(chunks)


def parse_header(raw):
    magic, version, kind, seq, block_id, length, crc, reserved = HEADER.unpack(raw)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {magic!r}")
    if version != VERSION:
        raise ProtocolError(f"unsupported version {version}")
    if kind not in (TYPE_FRAME, TYPE_CANDIDATES, TYPE_STATUS):
        raise ProtocolError(f"unknown type {kind}")
    if length > MAX_PAYLOAD or (kind == TYPE_STATUS and length > MAX_STATUS):
        raise ProtocolError(f"payload too large: {length}")
    if reserved != 0:
        raise ProtocolError("reserved field not zero")
    return kind, seq, block_id, length, crc


def decode_frame(payload):
    if len(payload) < 8:
        raise ProtocolError("frame payload too short")
    width, height = struct.unpack_from("<II", payload)
    if width == 0 or height == 0 or width * height != len(payload) - 8:
        raise ProtocolError(f"frame size mismatch: {width}x{height} vs {len(payload) - 8} bytes")
    return width, height, payload[8:]


def decode_candidates(payload):
    if len(payload) < 4:
        raise ProtocolError("candidate payload too short")
    (count,) = struct.unpack_from("<I", payload)
    if len(payload) != 4 + count * CANDIDATE.size:
        raise ProtocolError("candidate count does not match payload length")
    items = []
    for i in range(count):
        x0, y0, x1, y1, area, peak, cx, cy = CANDIDATE.unpack_from(payload, 4 + i * CANDIDATE.size)
        items.append({"bbox": [x0, y0, x1, y1], "area": area, "peak": peak,
                      "cx": round(cx, 3), "cy": round(cy, 3)})
    return items


def write_new(path, data):
    """先写临时文件再改名；目标已存在就报错，绝不覆盖。"""
    tmp = path.with_name(path.name + ".partial")
    with open(tmp, "xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    try:
        os.link(tmp, path)
    finally:
        tmp.unlink()


def store(out, kind, seq, block_id, payload):
    name = f"{seq:010d}_{block_id}"
    if kind == TYPE_FRAME:
        width, height, pixels = decode_frame(payload)
        path = out / "frames" / f"{name}.pgm"
        write_new(path, f"P5\n{width} {height}\n255\n".encode("ascii") + pixels)
        return f"frame {width}x{height} -> {path}"
    if kind == TYPE_CANDIDATES:
        items = decode_candidates(payload)
        doc = {"schema_version": "board-candidates-v0", "sequence": seq, "block_id": block_id,
               "bbox_convention": "half-open", "connectivity": 8, "decision": "unknown",
               "count": len(items), "candidates": items}
        path = out / "candidates" / f"{name}.json"
        write_new(path, json.dumps(doc, ensure_ascii=False, indent=1).encode("utf-8"))
        return f"{len(items)} candidates -> {path}"
    text = payload.decode("utf-8")
    json.loads(text)                     # 必须是合法 JSON
    if "\n" in text:
        raise ProtocolError("status must be a single line")
    with open(out / "status.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps({"sequence": seq, "block_id": block_id, "status": json.loads(text)},
                           ensure_ascii=False) + "\n")
    return "status"


def serve_connection(conn, out, log):
    last_seq = 0
    while True:
        raw = recv_exact(conn, HEADER_SIZE)
        if raw is None:
            return
        kind, seq, block_id, length, crc = parse_header(raw)
        if seq != last_seq + 1:
            raise ProtocolError(f"sequence jump {last_seq} -> {seq}")
        payload = recv_exact(conn, length) if length else b""
        if payload is None:
            raise ProtocolError("connection closed before payload")
        if zlib.crc32(payload) != crc:
            raise ProtocolError(f"CRC mismatch on message {seq}")
        last_seq = seq
        print(f"#{seq} block {block_id}: {store(out, kind, seq, block_id, payload)}", file=log)


def main(argv=None):
    parser = argparse.ArgumentParser(description="I6 receiver")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--out", type=Path, default=Path("runtime"))
    parser.add_argument("--once", action="store_true", help="处理完一个连接就退出（测试用）")
    args = parser.parse_args(argv)

    for sub in ("frames", "candidates"):
        (args.out / sub).mkdir(parents=True, exist_ok=True)

    with socket.create_server((args.host, args.port), reuse_port=False) as server:
        print(f"rx_tcp listening on {args.host}:{args.port}, writing to {args.out}", file=sys.stderr)
        while True:
            conn, peer = server.accept()
            print(f"connected: {peer[0]}:{peer[1]}", file=sys.stderr)
            error = None
            with conn:
                try:
                    serve_connection(conn, args.out, sys.stdout)
                except (ProtocolError, UnicodeDecodeError, ValueError, OSError) as e:
                    error = e
            print(f"disconnected: {error or 'clean close'}", file=sys.stderr)
            if args.once:
                return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
