"""颗粒候选检测的 Python 参考实现，和 board/processing/sg_candidates.c 同一个算法。

用途：在电脑上调参数、看中间结果、对真值（verify/V4）；定下来后板上 C 版本输出必须一致。
一致的含义：候选个数、顺序、外接框、面积、峰值完全相同；质心允许 1e-4 像素以内的浮点误差
（求和顺序不同）。

算法：
  1. signed = 像素 - 该列背景均值（float32）；没给背景就用本图逐列均值
  2. |signed| >= threshold 的像素算可疑
  3. 8 邻接连通域
  4. 外接框（半开）、面积、峰值（四舍五入）、以 |signed| 加权的质心；按面积过滤
  5. 按每个候选第一个像素的光栅顺序输出

实现上按"行内连续段（run）"做并查集，比逐像素快得多，结果和 C 的逐像素版本相同。

用法：python -I host/detect/candidates.py FRAME.pgm --threshold 25 [--background bg.csv] > cands.json
"""
import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np


def column_background(image):
    """本图逐列均值（和 C 的 self_background 相同：先精确求和，再除，再转 float32）。"""
    sums = image.astype(np.int64).sum(axis=0)
    return (sums.astype(np.float64) / image.shape[0]).astype(np.float32)


def load_background(path, width):
    """读 V3 输出的 x,mean,sigma CSV。"""
    mean = np.zeros(width, dtype=np.float32)
    seen = 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            mean[int(row["x"])] = float(row["mean"])
            seen += 1
    if seen != width:
        raise ValueError(f"background has {seen} columns, image has {width}")
    return mean


def find_candidates(image, threshold, min_area=2, max_area=0, max_candidates=0, mean=None):
    """返回 (候选列表, 是否截断)。每个候选是 dict：bbox [x0,y0,x1,y1]、area、peak、cx、cy。"""
    if image.dtype != np.uint8 or image.ndim != 2:
        raise ValueError("Mono8 2-D array required")
    if not 1 <= threshold <= 255 or min_area < 1 or (max_area and max_area < min_area):
        raise ValueError("invalid parameters")
    height, width = image.shape
    if mean is None:
        mean = column_background(image)
    amp = np.abs(image.astype(np.float32) - mean.astype(np.float32)[None, :])
    mask = amp >= np.float32(threshold)

    parent = []                    # 每个 run 一个编号
    runs = []                      # (y, x0, x1)

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    prev = []                      # 上一行的 (x0, x1, run_id)
    for y in range(height):
        row = mask[y]
        if not row.any():
            prev = []
            continue
        edges = np.flatnonzero(np.diff(np.concatenate(([0], row.view(np.uint8), [0]))))
        cur = []
        j = 0
        for x0, x1 in zip(edges[0::2].tolist(), edges[1::2].tolist()):
            rid = len(runs)
            runs.append((y, x0, x1))
            parent.append(rid)
            # 8 邻接：上一行 [a, b) 和本段 [x0, x1) 只要 a <= x1 且 b >= x0 就相连
            while j < len(prev) and prev[j][1] < x0:
                j += 1
            k = j
            while k < len(prev) and prev[k][0] <= x1:
                ra, rb = find(rid), find(prev[k][2])
                if ra != rb:
                    lo, hi = min(ra, rb), max(ra, rb)
                    parent[hi] = lo        # 小号做根
                k += 1
            cur.append((x0, x1, rid))
        prev = cur

    # 按根汇总统计
    stats = {}
    for rid, (y, x0, x1) in enumerate(runs):
        root = find(rid)
        a = amp[y, x0:x1].astype(np.float64)
        xs = np.arange(x0, x1, dtype=np.float64)
        s = stats.get(root)
        if s is None:
            s = stats[root] = {"x0": x0, "y0": y, "x1": x1, "y1": y + 1, "area": 0,
                               "peak": 0.0, "w": 0.0, "wx": 0.0, "wy": 0.0}
        s["x0"] = min(s["x0"], x0)
        s["x1"] = max(s["x1"], x1)
        s["y1"] = max(s["y1"], y + 1)
        s["area"] += x1 - x0
        s["peak"] = max(s["peak"], float(amp[y, x0:x1].max()))
        s["w"] += float(a.sum())
        s["wx"] += float((a * xs).sum())
        s["wy"] += float(a.sum()) * y

    out, truncated = [], False
    limit = max_candidates or None
    for root in sorted(stats):
        s = stats[root]
        if s["area"] < min_area or (max_area and s["area"] > max_area):
            continue
        if limit is not None and len(out) >= limit:
            truncated = True
            break
        w = s["w"]
        out.append({
            "bbox": [s["x0"], s["y0"], s["x1"], s["y1"]],
            "area": s["area"],
            "peak": int(math.floor(s["peak"] + 0.5)),
            "cx": float(np.float32(s["wx"] / w)) if w > 0 else float(s["x0"]),
            "cy": float(np.float32(s["wy"] / w)) if w > 0 else float(s["y0"]),
        })
    return out, truncated


def main(argv=None):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "verify"))
    from pgmio import read_pgm

    parser = argparse.ArgumentParser(description="Python reference of sg_candidates")
    parser.add_argument("pgm", type=Path)
    parser.add_argument("--threshold", type=int, default=25)
    parser.add_argument("--min-area", type=int, default=2)
    parser.add_argument("--max-area", type=int, default=0)
    parser.add_argument("--max-candidates", type=int, default=100000)
    parser.add_argument("--background", type=Path, help="V3 --save-background 输出的 CSV")
    args = parser.parse_args(argv)

    image = read_pgm(args.pgm)
    mean = load_background(args.background, image.shape[1]) if args.background else None
    items, truncated = find_candidates(image, args.threshold, args.min_area, args.max_area,
                                       args.max_candidates, mean)
    json.dump({"schema_version": "board-candidates-v0", "source": str(args.pgm),
               "bbox_convention": "half-open", "connectivity": 8, "decision": "unknown",
               "threshold": args.threshold, "min_area": args.min_area, "max_area": args.max_area,
               "truncated": truncated, "count": len(items), "candidates": items},
              sys.stdout, ensure_ascii=False, indent=1)
    print()
    if truncated:
        print(f"WARNING: truncated at {len(items)} candidates", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
