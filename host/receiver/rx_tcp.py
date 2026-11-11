"""rx_tcp · 电脑端接收板子经网口发来的数据（I6 协议）。

职责：监听端口 → 按 32 字节头拆消息 → 校验 → 落盘
  类型 1 原图块 → OUT/frames/<序号>_<block_id>.pgm
  类型 2 候选   → OUT/candidates/<序号>_<block_id>.json
  类型 3 状态   → 追加到 OUT/status.jsonl
不做任何检测。一次只接一个板子连接；连接断了就等下一个。
中控台里用的是同一份代码（Receiver 类，后台线程）。
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
import threading
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
    """返回 (候选列表, 是否截断)。负载：u32 count、u32 flags（bit0 截断）、count × 32 字节。"""
    if len(payload) < 8:
        raise ProtocolError("candidate payload too short")
    count, flags = struct.unpack_from("<II", payload)
    if flags & ~1:
        raise ProtocolError(f"unknown candidate flags 0x{flags:x}")
    if len(payload) != 8 + count * CANDIDATE.size:
        raise ProtocolError("candidate count does not match payload length")
    items = []
    for i in range(count):
        x0, y0, x1, y1, area, peak, cx, cy = CANDIDATE.unpack_from(payload, 8 + i * CANDIDATE.size)
        items.append({"bbox": [x0, y0, x1, y1], "area": area, "peak": peak,
                      "cx": round(cx, 3), "cy": round(cy, 3)})
    return items, bool(flags & 1)


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


class Layout:
    """落盘位置。命令行默认 OUT/frames、OUT/candidates、OUT/status.jsonl；
    中控台把原图直接放进采集目录（和串口回传的图放在一起）。"""

    def __init__(self, frames, candidates, status):
        self.frames, self.candidates, self.status = Path(frames), Path(candidates), Path(status)
        self.frames.mkdir(parents=True, exist_ok=True)
        self.candidates.mkdir(parents=True, exist_ok=True)

    @classmethod
    def under(cls, out):
        out = Path(out)
        return cls(out / "frames", out / "candidates", out / "status.jsonl")


def store(layout, kind, seq, block_id, payload, prefix=""):
    """校验并落盘一条消息，返回 (说明文字, 落盘路径或 None)。"""
    name = f"{prefix}{seq:010d}_{block_id}"
    if kind == TYPE_FRAME:
        width, height, pixels = decode_frame(payload)
        path = layout.frames / f"{name}.pgm"
        write_new(path, f"P5\n{width} {height}\n255\n".encode("ascii") + pixels)
        return f"frame {width}x{height} -> {path}", path
    if kind == TYPE_CANDIDATES:
        items, truncated = decode_candidates(payload)
        doc = {"schema_version": "board-candidates-v0", "sequence": seq, "block_id": block_id,
               "bbox_convention": "half-open", "connectivity": 8, "decision": "unknown",
               "truncated": truncated, "count": len(items), "candidates": items}
        path = layout.candidates / f"{name}.json"
        write_new(path, json.dumps(doc, ensure_ascii=False, indent=1).encode("utf-8"))
        return f"{len(items)} candidates{' (TRUNCATED)' if truncated else ''} -> {path}", path
    text = payload.decode("utf-8")
    if "\n" in text:
        raise ProtocolError("status must be a single line")
    status = json.loads(text)                     # 必须是合法 JSON
    with open(layout.status, "a", encoding="utf-8") as f:
        f.write(json.dumps({"sequence": seq, "block_id": block_id, "status": status},
                           ensure_ascii=False) + "\n")
    return "status", None


def serve_connection(conn, layout, on_message, prefix=""):
    """处理一个连接直到对方正常关闭。on_message(kind, seq, block_id, 说明, 路径)。"""
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
        text, path = store(layout, kind, seq, block_id, payload, prefix)
        on_message(kind, seq, block_id, text, path)


class Receiver:
    """后台线程版，给中控台用：start() 开始监听，stop() 停止。一次服务一个连接。"""

    def __init__(self, layout, on_message, on_event=lambda text: None):
        self.layout, self.on_message, self.on_event = layout, on_message, on_event
        self.server = None
        self.thread = None
        self.connections = 0

    def start(self, host="0.0.0.0", port=8770):
        if self.server is not None:
            return
        self.server = socket.create_server((host, port))
        self.server.settimeout(0.5)
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def stop(self):
        server, self.server = self.server, None
        if server is not None:
            server.close()
        if self.thread is not None:
            self.thread.join(timeout=2)

    @property
    def running(self):
        return self.server is not None

    def _loop(self):
        while self.server is not None:
            try:
                conn, peer = self.server.accept()
            except (socket.timeout, OSError):
                continue
            self.connections += 1
            prefix = f"net{self.connections:04d}-"
            with conn:
                conn.settimeout(30)
                try:
                    serve_connection(conn, self.layout, self.on_message, prefix)
                    self.on_event(f"网口连接 {peer[0]} 正常结束。")
                except (ProtocolError, UnicodeDecodeError, ValueError, OSError) as e:
                    self.on_event(f"网口连接 {peer[0]} 出错断开：{e}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="I6 receiver")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--out", type=Path, default=Path("runtime"))
    parser.add_argument("--once", action="store_true", help="处理完一个连接就退出（测试用）")
    args = parser.parse_args(argv)

    layout = Layout.under(args.out)

    def show(kind, seq, block_id, text, path):
        print(f"#{seq} block {block_id}: {text}", flush=True)

    with socket.create_server((args.host, args.port), reuse_port=False) as server:
        print(f"rx_tcp listening on {args.host}:{args.port}, writing to {args.out}", file=sys.stderr)
        while True:
            conn, peer = server.accept()
            print(f"connected: {peer[0]}:{peer[1]}", file=sys.stderr)
            error = None
            with conn:
                try:
                    serve_connection(conn, layout, show)
                except (ProtocolError, UnicodeDecodeError, ValueError, OSError) as e:
                    error = e
            print(f"disconnected: {error or 'clean close'}", file=sys.stderr)
            if args.once:
                return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
