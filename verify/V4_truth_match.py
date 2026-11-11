"""V4 · 真值比对：已知颗粒能检出几个、误报几个、位置准不准？

这是唯一直接回答"检测系统对不对"的验证，也是验收要拿出来的数字。

输入：
  - 候选：board-candidates-v0 JSON（host/detect/candidates.py 或 rx_tcp 收到的），
          里面 candidates[i] 有 cx、cy（像素）
  - 真值 CSV：表头 x,y,size_um（像素坐标和颗粒尺寸）；可选 id 列
输出：召回率、误报数、位置误差（均值 / 95% / 最大），召回率按尺寸分档（≥30 µm / 10–30 / <10）。

匹配规则：所有 (真值, 候选) 对按距离从小到大贪心配对，一一对应，距离 < radius 才算命中。
  （贪心不是全局最优，颗粒稀疏时两者一样；颗粒很密时再换匈牙利算法。）

用法：python -I verify/V4_truth_match.py CANDIDATES.json TRUTH.csv --radius 5
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


def match(truth, cands, radius):
    """返回 [(真值下标, 候选下标, 距离)]。"""
    pairs = []
    for i, (tx, ty, _) in enumerate(truth):
        for j, (cx, cy) in enumerate(cands):
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
    args = parser.parse_args(argv)

    cands = load_candidates(args.candidates)
    truth = load_truth(args.truth)
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
