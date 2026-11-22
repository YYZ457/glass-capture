"""V4 · 真值比对：已知颗粒能检出几个、误报几个、位置准不准？

这是唯一直接回答"检测系统对不对"的验证，也是验收要拿出来的数字。

输入：
  - 候选：board-candidates-v0 JSON（host/detect/candidates.py 或 rx_tcp 收到的），
          里面 candidates[i] 有 cx、cy（像素）
  - 真值 CSV：表头 x,y,size_um（像素坐标和颗粒尺寸）；可选 id 列
  - 可选 --roi：只统计这些多边形里的候选（verify/plate_register.py 给标定板颗粒区出的 JSON），
          区外的点阵、行标、调焦条之类不算误报
输出：召回率、误报数、位置误差（均值 / 95% / 最大），召回率按尺寸分档（≥30 µm / 10–30 / <10），
      尺寸种类不多（≤ 12 种，比如标定板）时再按每种尺寸列一遍。

匹配规则：所有 (真值, 候选) 对按距离从小到大贪心配对，一一对应，距离 < radius 才算命中。
  （贪心不是全局最优，颗粒稀疏时两者一样；颗粒很密时再换匈牙利算法。）

用法：python -I verify/V4_truth_match.py CANDIDATES.json TRUTH.csv --radius 5 [--roi roi.json]
"""
import argparse
import csv
import json
import math
import sys

BINS = [("≥30 µm", 30, math.inf), ("10–30 µm", 10, 30), ("<10 µm", 0, 10)]


def load_candidates(path):
    doc = json.load(open(path, encoding="utf-8"))
    items = doc["candidates"] if isinstance(doc, dict) else doc
    return [(float(c["cx"]), float(c["cy"])) for c in items]


def load_truth(path):
    with open(path, newline="", encoding="utf-8") as f:
        return [(float(r["x"]), float(r["y"]), float(r["size_um"])) for r in csv.DictReader(f)]


def load_roi(path):
    """[[(u, v), ...], ...]：每个多边形的顶点（像素）。"""
    doc = json.load(open(path, encoding="utf-8"))
    return [[(float(u), float(v)) for u, v in p["polygon"]] for p in doc["polygons"]]


def inside(poly, x, y):
    """射线法判断点是否在多边形内。"""
    hit = False
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


def match(truth, cands, radius):
    """返回 [(真值下标, 候选下标, 距离)]。候选按 radius 大小的格子分桶，只比相邻格子，几千对几千也很快。"""
    cell = max(radius, 1e-9)
    buckets = {}
    for j, (cx, cy) in enumerate(cands):
        buckets.setdefault((int(cx // cell), int(cy // cell)), []).append(j)
    pairs = []
    for i, (tx, ty, _) in enumerate(truth):
        gx, gy = int(tx // cell), int(ty // cell)
        for bx in (gx - 1, gx, gx + 1):
            for by in (gy - 1, gy, gy + 1):
                for j in buckets.get((bx, by), ()):
                    cx, cy = cands[j]
                    d = math.hypot(tx - cx, ty - cy)
                    if d < radius:
                        pairs.append((d, i, j))
    pairs.sort()
    used_t, used_c, out = set(), set(), []
    for d, i, j in pairs:
        if i not in used_t and j not in used_c:
            used_t.add(i)
            used_c.add(j)
            out.append((i, j, d))
    return out


def percentile(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(math.ceil(p / 100 * len(s))) - 1)]


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("candidates")
    parser.add_argument("truth")
    parser.add_argument("--radius", type=float, default=5.0)
    parser.add_argument("--roi", help="只统计这些多边形里的候选（plate_register.py --roi 的输出）")
    args = parser.parse_args(argv)

    cands = load_candidates(args.candidates)
    truth = load_truth(args.truth)
    if args.roi:
        polys = load_roi(args.roi)
        before = len(cands)
        cands = [c for c in cands if any(inside(p, *c) for p in polys)]
        print(f"roi: {len(polys)} polygons, candidates {before} -> {len(cands)} inside")
    pairs = match(truth, cands, args.radius)
    hit_truth = {i for i, _, _ in pairs}
    errors = [d for _, _, d in pairs]

    print(f"truth {len(truth)}, candidates {len(cands)}, matched {len(pairs)} (radius {args.radius} px)")
    print(f"recall {len(pairs) / len(truth):.3f}" if truth else "recall n/a (no truth)")
    print(f"false alarms {len(cands) - len(pairs)}")
    print(f"position error  mean {sum(errors) / len(errors):.2f}  p95 {percentile(errors, 95):.2f}  "
          f"max {max(errors):.2f} px" if errors else "position error n/a")
    for name, lo, hi in BINS:
        idx = [i for i, t in enumerate(truth) if lo <= t[2] < hi]
        if idx:
            hits = sum(1 for i in idx if i in hit_truth)
            print(f"  {name:10s} {hits}/{len(idx)}  recall {hits / len(idx):.3f}")
        else:
            print(f"  {name:10s} no truth particles")
    sizes = sorted({t[2] for t in truth})
    if 1 < len(sizes) <= 12:
        print("by size:")
        for size in sizes:
            idx = [i for i, t in enumerate(truth) if t[2] == size]
            hits = sum(1 for i in idx if i in hit_truth)
            print(f"  {size:g} um  {hits}/{len(idx)}  recall {hits / len(idx):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
