"""V2 · 行完整性：有没有丢行、错位、宽度被切分？

回答的问题：
  1. 宽度到底是 4096 还是 16384？（遮住传感器左半边采一帧：
     每行都有明暗分界 → 相机输出 4096；每 4 行一组明暗交替 → 相机发 16384、卡按 4096 切行）
  2. 相邻行之间有没有突然的错位（列方向平移）？
  3. 连续多块之间块号是否连续（读 SGJSON 日志的 block_id）？

不需要镜头，不需要测试图案，现在就能做。

用法：
  python -I verify/V2_line_integrity.py FRAME.pgm            问题 1、2
  python -I verify/V2_line_integrity.py --log stream.log     问题 3
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgmio import read_pgm  # noqa: E402

MAX_SHIFT = 32


def width_split_test(image):
    """判断 '每行有分界' 还是 '每 4 行一组交替'。返回结论字符串。"""
    height, width = image.shape
    rows = image.astype(np.float64)
    left = rows[:, : width // 2].mean(axis=1)
    right = rows[:, width // 2:].mean(axis=1)
    row_mean = rows.mean(axis=1)

    contrast = float(np.median(np.abs(right - left)))
    lo, hi = float(row_mean.min()), float(row_mean.max())
    if hi - lo < 10 and contrast < 10:
        return "no clear dark/bright structure (did you cover half the sensor?)"
    if contrast >= 10 and contrast >= (hi - lo):
        return f"every row has a left/right boundary (median contrast {contrast:.1f}) -> camera outputs {width} per line"

    bright = row_mean > (lo + hi) / 2
    groups = {}
    for period in (2, 4, 8):
        n = height // period * period
        if n < period * 2:
            continue
        g = bright[:n].reshape(-1, period)
        consistent = bool((g == g[0]).all())
        groups[period] = consistent
        if consistent and g[0].any() and not g[0].all():
            pattern = "".join("B" if b else "d" for b in g[0])
            return (f"rows alternate with period {period} (pattern {pattern}) -> "
                    f"one sensor line is split into {period} rows of {width}")
    return f"unclear: row means range {lo:.1f}..{hi:.1f}, left/right contrast {contrast:.1f}"


def row_shifts(image):
    """相邻两行之间最可能的列平移（互相关最大的偏移）。返回非零平移的 [(行号, 平移)]。"""
    rows = image.astype(np.float64)
    rows -= rows.mean(axis=1, keepdims=True)
    found = []
    for y in range(1, rows.shape[0]):
        a, b = rows[y - 1], rows[y]
        best, best_shift = None, 0
        for s in range(-MAX_SHIFT, MAX_SHIFT + 1):
            if s >= 0:
                score = float(np.dot(a[s:], b[: len(b) - s]))
            else:
                score = float(np.dot(a[: len(a) + s], b[-s:]))
            if best is None or score > best:
                best, best_shift = score, s
        if best_shift != 0 and best > 0:
            found.append((y, best_shift))
    return found


def check_log(path):
    ids = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("SGJSON "):
            try:
                d = json.loads(line[7:])
            except ValueError:
                continue
            if d.get("event") == "result":
                ids.append((int(d["sequence"]), d["block_id"]))
    if not ids:
        print("no result lines found")
        return 1
    # 注意：tx_jsonl 对 UNKNOWN 结果抽样输出，sequence 不连续是正常的；
    # 只有 sequence 连续的相邻两行，才能用来判断 block_id 是否 +1。
    checked = jumps = 0
    for (s0, b0), (s1, b1) in zip(ids, ids[1:]):
        if s1 == s0 + 1:
            checked += 1
            if b1 != (b0 + 1) & 0xFFFFFFFF:
                jumps += 1
                print(f"  block id jump after sequence {s0}: {b0} -> {b1}")
    print(f"{len(ids)} result lines, {checked} consecutive pairs checked, {jumps} block id jumps")
    if checked == 0:
        print("  (run sg3000-stream with --emit-ms small enough that every block is printed)")
    return 1 if jumps else 0


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("pgm", nargs="?")
    parser.add_argument("--log", help="sg3000-stream 输出的 SGJSON 日志")
    parser.add_argument("--max-rows", type=int, default=2000, help="错位检查最多看多少行（很慢）")
    args = parser.parse_args(argv)
    if args.log:
        return check_log(args.log)
    if not args.pgm:
        parser.error("need a PGM or --log")

    image = read_pgm(args.pgm)
    print(f"{args.pgm}: {image.shape[1]} x {image.shape[0]}")
    print("width test:", width_split_test(image))
    shifts = row_shifts(image[: args.max_rows])
    if shifts:
        print(f"row shifts: {len(shifts)} rows look shifted vs. the previous row, first: {shifts[:10]}")
    else:
        print(f"row shifts: none in the first {min(args.max_rows, image.shape[0])} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
