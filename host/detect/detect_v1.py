"""检测算法第二版的设计原型（docs/design/detection_plan.md 第 5 节），只在电脑上跑。

注意：这不是板上 sg_candidates 的参考实现（那个是 host/detect/candidates.py，两边必须逐项一致）。
这里是"下一版打算怎么做"的可运行草稿，用来在仿真图和以后的实拍图上验证思路、定参数。
等参数定了，再把它变成新的板上 C 版本和新的一致性合同。

和第一版的区别：
  1. 阈值按列给：每列一对 lo / hi（8 位整数表）。像素 <= lo 算暗点，>= hi 算亮点。
     delta[x] = max(k × 该列噪声, rel × 该列背景, floor)；lo = bg - delta，hi = bg + delta。
     —— 每个像素只做两次 8 位比较，板上可以用 NEON 一次比 16 个像素（方案第 4 节算力预算）。
  2. 亮、暗分开做连通域（明场下颗粒是暗的，暗场下是亮的；两种都保留，由后面分类决定要哪种）。
  3. 只对候选周围的小窗口算特征（候选很少，这一步几乎不花时间）：
       volume   = Σ 相对对比度      → 等效直径 d_vol（对模糊不敏感：模糊只是把同样的"挡光量"摊开）
       fwhm     = 半高处的面积      → 直径 d_fwhm（颗粒 >= 3 像素时可靠，和透明度无关）
       edge_px  = 25% 和 75% 等高线的半径差 → 边缘有多糊（区分正面 / 背面 / 离焦的灰尘）
       elong    = 长短轴之比        → 区分颗粒和划痕 / 纤维
  4. 外接框相距 3 个像素以内的碎块并成一个候选（模糊、低对比的颗粒在阈值附近会碎）。
  5. 规则分类：noise / particle / blurred / elongated / large。

用法：python -I host/detect/detect_v1.py SCENE.pgm --clean CLEAN.pgm [--mode bright|dark] > cands.json
     输出里每个候选有 cx、cy，可以直接喂给 verify/V4_truth_match.py。
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

OBJECT_PIXEL_UM = 80000.0 / 16384      # D002：视场 80 mm / 16384 像素
ROI_PAD = 4                            # 特征窗口 = 外接框向外扩 4 个像素


def calibrate(clean):
    """从一张干净图（没有颗粒、同样的光照和相机设置）得到逐列背景和逐列噪声。"""
    pixels = clean.astype(np.float64)
    return pixels.mean(axis=0), pixels.std(axis=0)


def column_thresholds(bg, sigma, k=6.0, rel=0.06, floor_dn=3.0):
    """每列一对 8 位阈值。lo = -1 / hi = 256 表示这一列这个方向永远不报。"""
    delta = np.maximum(np.maximum(k * sigma, rel * bg), floor_dn)
    lo = np.floor(bg - delta).astype(np.int32)
    hi = np.ceil(bg + delta).astype(np.int32)
    return np.maximum(lo, -1), np.minimum(hi, 256)


def label_mask(mask):
    """8 邻接连通域（按行内连续段做并查集，和 candidates.py 同一个办法）。

    返回 [(y0, [(y, x0, x1), ...]), ...]，按第一个像素的光栅顺序排列。
    """
    parent, runs = [], []

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    prev, prev_y = [], -2
    for y in np.flatnonzero(mask.any(axis=1)).tolist():
        if y != prev_y + 1:
            prev = []
        row = mask[y]
        edges = np.flatnonzero(np.diff(np.concatenate(([0], row.view(np.uint8), [0]))))
        cur, j = [], 0
        for x0, x1 in zip(edges[0::2].tolist(), edges[1::2].tolist()):
            rid = len(runs)
            runs.append((y, x0, x1))
            parent.append(rid)
            while j < len(prev) and prev[j][1] < x0:
                j += 1
            k = j
            while k < len(prev) and prev[k][0] <= x1:
                ra, rb = find(rid), find(prev[k][2])
                if ra != rb:
                    parent[max(ra, rb)] = min(ra, rb)
                k += 1
            cur.append((x0, x1, rid))
        prev, prev_y = cur, y
    groups = {}
    for rid, run in enumerate(runs):
        groups.setdefault(find(rid), []).append(run)
    return [groups[root] for root in sorted(groups)]


def merge_near(components, gap):
    """把外接框相距不超过 gap 个像素的连通域并成一个（模糊、低对比的颗粒在阈值附近会碎成几块）。"""
    if gap <= 0 or len(components) < 2:
        return components
    boxes = [(min(r[1] for r in c), min(r[0] for r in c), max(r[2] for r in c), max(r[0] for r in c) + 1)
             for c in components]
    parent = list(range(len(components)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    order = sorted(range(len(boxes)), key=lambda i: boxes[i][1])
    for a_pos, a in enumerate(order):
        ax0, ay0, ax1, ay1 = boxes[a]
        for b in order[a_pos + 1:]:
            bx0, by0, bx1, by1 = boxes[b]
            if by0 > ay1 + gap + 64:        # 按 y0 排过序；留 64 行余量给很高的连通域
                break
            if bx0 <= ax1 + gap and ax0 <= bx1 + gap and by0 <= ay1 + gap and ay0 <= by1 + gap:
                ra, rb = find(a), find(b)
                if ra != rb:
                    parent[max(ra, rb)] = min(ra, rb)
    groups = {}
    for i, c in enumerate(components):
        groups.setdefault(find(i), []).extend(c)
    return [groups[root] for root in sorted(groups)]


def _features(image, bg, black, runs, polarity, pixel_um):
    h, w = image.shape
    ys = [r[0] for r in runs]
    x0 = min(r[1] for r in runs)
    x1 = max(r[2] for r in runs)
    y0, y1 = min(ys), max(ys) + 1
    area = sum(r[2] - r[1] for r in runs)
    ry0, ry1 = max(y0 - ROI_PAD, 0), min(y1 + ROI_PAD, h)
    rx0, rx1 = max(x0 - ROI_PAD, 0), min(x1 + ROI_PAD, w)
    roi = image[ry0:ry1, rx0:rx1].astype(np.float64)
    base = bg[rx0:rx1][None, :]
    if polarity == "dark":
        c = (base - roi) / np.maximum(base - black, 1.0)     # 相对对比度：1 = 完全挡光
    else:
        c = roi - base                                       # 暗场：直接用灰度（没有"满刻度"可除）
    diff_peak = float(np.abs(roi - base).max())
    peak = float(c.max())
    pos = np.clip(c, 0.0, None)
    volume = float(pos[pos >= 0.1 * peak].sum()) if peak > 0 else 0.0   # 只累加明显属于颗粒的部分，少吃噪声
    half_area = int((c >= 0.5 * peak).sum()) if peak > 0 else 0
    a25 = int((c >= 0.25 * peak).sum()) if peak > 0 else 0
    a75 = int((c >= 0.75 * peak).sum()) if peak > 0 else 0
    edge = math.sqrt(a25 / math.pi) - math.sqrt(a75 / math.pi)

    wgt = np.where(c >= 0.25 * peak, pos, 0.0) if peak > 0 else pos
    total = wgt.sum()
    yy, xx = np.mgrid[ry0:ry1, rx0:rx1]
    if total > 0:
        cx = float((wgt * xx).sum() / total)
        cy = float((wgt * yy).sum() / total)
        mxx = float((wgt * (xx - cx) ** 2).sum() / total) + 1.0 / 12
        myy = float((wgt * (yy - cy) ** 2).sum() / total) + 1.0 / 12
        mxy = float((wgt * (xx - cx) * (yy - cy)).sum() / total)
        root = math.sqrt(max(((mxx - myy) / 2) ** 2 + mxy ** 2, 0.0))
        major, minor = (mxx + myy) / 2 + root, max((mxx + myy) / 2 - root, 1e-6)
        elong = math.sqrt(major / minor)
    else:
        cx, cy, elong = (x0 + x1 - 1) / 2.0, (y0 + y1 - 1) / 2.0, 1.0
    out = {
        "bbox": [x0, y0, x1, y1], "area": area, "polarity": polarity,
        "peak": int(math.floor(diff_peak + 0.5)), "cx": cx, "cy": cy,
        "peak_contrast": round(peak, 4), "volume": round(volume, 3),
        "d_fwhm_um": round(2.0 * math.sqrt(half_area / math.pi) * pixel_um, 2),
        "edge_px": round(edge, 3), "elongation": round(elong, 3),
    }
    if polarity == "dark":
        out["d_vol_um"] = round(2.0 * math.sqrt(volume / math.pi) * pixel_um, 2)
    return out


def classify(c, min_area=2, max_area=4000, edge_limit_px=1.75, elong_limit=3.0):
    """规则分类（阈值是仿真下的初值，实拍后要重定）。"""
    if c["area"] < min_area:
        return "noise"
    if c["area"] > max_area:
        return "large"
    if c["elongation"] > elong_limit and c["area"] >= 6:
        return "elongated"
    if c["edge_px"] > edge_limit_px:
        return "blurred"
    return "particle"


def detect(image, bg, sigma, mode="bright", k=6.0, rel=0.06, floor_dn=3.0, min_area=2,
           black=2.0, pixel_um=OBJECT_PIXEL_UM, edge_limit_px=1.75, merge_gap=3):
    """返回候选列表（每个是 dict）。mode=bright 只找暗点，dark 只找亮点，both 两种都找。"""
    if image.dtype != np.uint8 or image.ndim != 2 or image.shape[1] != len(bg):
        raise ValueError("Mono8 image with the same width as the calibration required")
    lo, hi = column_thresholds(bg, sigma, k, rel, floor_dn)
    out = []
    wanted = {"bright": ["dark"], "dark": ["bright"], "both": ["dark", "bright"]}[mode]
    for polarity in wanted:
        mask = image <= lo[None, :] if polarity == "dark" else image >= hi[None, :]
        for runs in merge_near(label_mask(mask), merge_gap):
            c = _features(image, bg, black, runs, polarity, pixel_um)
            c["class"] = classify(c, min_area=min_area, edge_limit_px=edge_limit_px)
            out.append(c)
    out.sort(key=lambda c: (c["bbox"][1], c["bbox"][0]))
    return out


def main(argv=None):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "verify"))
    from pgmio import read_pgm

    parser = argparse.ArgumentParser(description="Detector v1 design prototype (host only)")
    parser.add_argument("pgm", type=Path)
    parser.add_argument("--clean", type=Path, required=True, help="同样设置下的干净图，用来标定背景和噪声")
    parser.add_argument("--mode", choices=["bright", "dark", "both"], default="bright")
    parser.add_argument("--k", type=float, default=6.0)
    parser.add_argument("--rel", type=float, default=0.06)
    parser.add_argument("--min-area", type=int, default=2)
    parser.add_argument("--keep", default="particle,blurred,elongated,large", help="输出哪些类别")
    args = parser.parse_args(argv)

    bg, sigma = calibrate(read_pgm(args.clean))
    items = detect(read_pgm(args.pgm), bg, sigma, args.mode, args.k, args.rel, min_area=args.min_area)
    keep = set(args.keep.split(","))
    items = [c for c in items if c["class"] in keep]
    json.dump({"schema_version": "detect-v1-study", "source": str(args.pgm), "bbox_convention": "half-open",
               "mode": args.mode, "k": args.k, "rel": args.rel, "count": len(items), "candidates": items},
              sys.stdout, ensure_ascii=False, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
