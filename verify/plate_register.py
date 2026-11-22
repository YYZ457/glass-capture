"""配准：把标定板 GC-CAL-01 的真值从"板上毫米"换成"图上像素"，交给 V4 比对。

为什么需要：厂家给的真值是"板上 (x, y) mm 处有一个 30 µm 的点"，检测程序输出的是"图上第几列、第几行"。
两者之间隔着镜头放大率和畸变、板子放歪的角度、有没有翻面掉头、玻璃走得快慢（扫描方向一行多长）。
这些都没法事先知道，只能从图里量：B 区点阵尺的位置是光刻保证的（±1 µm），量出它们在图上的位置，
就能解出换算关系。速度不匀会让离尺子远的地方越偏越多，所以颗粒区每一行两端还有行标，逐行再定一次。

步骤：
  1. 每列背景亮度 → 阈值（明场：铬点是暗的）→ 连通域，量每个暗斑的宽、高、填充率、加权质心。
  2. 按横向宽度（不受速度影响）和形状分类：Ø0.2 mm 点阵点、Ø0.4 mm 刻度点、0.4 mm 方块、Ø0.1 mm 行标。
  3. 点阵：从近邻算出两个格向量，以方块为原点编号；编号的正负号给出有没有翻面 / 掉头。
     拟合 列 = 三次多项式(x) + 旋转项，行 = 仿射。得到每像素多少微米、畸变、旋转、扫描方向一行多长。
  4. 行标：从点阵最后一行出发逐行往下找，每找到一行就按实测间距更新"每毫米多少行"，所以能跟住速度变化。
  5. 每个颗粒按所在行的两个行标定位：行内横向用点阵的畸变曲线，纵向在两个行标之间线性插值（板子歪了一点时行是斜的）。

输出：
  - 像素真值 CSV（表头 x,y,size_um，V4 直接能读；另带 id、板坐标、该行定位状态）
  - 颗粒区多边形 JSON（--roi）：V4 用 --roi 只统计颗粒区里的误报，点阵、行标、调焦条不算
  - 报告 JSON（--report）和屏幕摘要：像素尺寸、畸变、旋转、方向、速度是否匹配、残差

只适用于明场（背光透射）图：铬点是暗的。暗场下铬点不亮，配准用明场扫一遍即可，换照明前后板子别动。

用法：
  python -I verify/plate_register.py SCAN.pgm --out truth_px.csv --roi roi.json --report report.json
  python -I verify/V4_truth_match.py candidates.json truth_px.csv --roi roi.json
选项：--pixel-um 4.88（标称物方像素，只用来分类，±25% 内都行） --truth（默认仓库里的 GC-CAL-01_truth_mm.csv）
依赖：numpy、scipy（连通域）。
"""
import argparse
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgmio import read_pgm  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRUTH = ROOT / "docs/design/calib_target/GC-CAL-01_truth_mm.csv"


class RegisterError(RuntimeError):
    """配准失败；消息说明卡在哪一步、可能的原因。"""


# ---------------------------------------------------------------- 真值

def load_plate_truth(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows.append(dict(id=int(r["id"]), zone=r["zone"], kind=r["kind"], x=float(r["x_mm"]),
                             y=float(r["y_mm"]), size_um=float(r["size_um"]), note=r.get("note", "")))
    need = {"grid", "orient_square", "anchor", "particle"}
    have = {r["kind"] for r in rows}
    if not need <= have:
        raise RegisterError(f"真值文件缺少 {sorted(need - have)}；请用 tools/calib_target.py 重新生成。")
    return rows


# ---------------------------------------------------------------- 找暗斑

def column_background(img, samples=512):
    """每列的背景亮度：均匀抽样若干行取 90% 分位（铬图案只占少数行，不影响）。"""
    idx = np.linspace(0, img.shape[0] - 1, min(img.shape[0], samples)).astype(int)
    return np.percentile(img[idx], 90, axis=0).astype(np.float32)


def find_blobs(img, bg, frac=0.5, strip=2048, overlap=512, max_w=600, max_h=1500):
    """暗于 frac × 背景的连通域。按条带处理，整张 16384 × 2 万行的图也不用一次性占满内存。"""
    from scipy import ndimage

    h, w = img.shape
    thr = np.clip(bg * frac, 1, 255).astype(np.uint8)
    blobs = []
    for y0 in range(0, h, strip):
        y1 = min(h, y0 + strip + overlap)
        sub = img[y0:y1]
        lab, n = ndimage.label(sub < thr[None, :])
        if not n:
            continue
        for k, sl in enumerate(ndimage.find_objects(lab), 1):
            ys, xs = sl
            if ys.start >= strip and y1 < h:            # 起点在重叠区：留给下一个条带
                continue
            if ys.start == 0 and y0 > 0:                # 碰到条带上沿：是上一个条带里那个斑的下半截
                continue
            if ys.stop >= sub.shape[0] and y1 < h:      # 碰到条带底：太长（调焦条、黑条），不要
                continue
            bh, bw = ys.stop - ys.start, xs.stop - xs.start
            if bw < 3 or bh < 2 or bw > max_w or bh > max_h:
                continue
            area = int(np.count_nonzero(lab[sl] == k))
            pad = 3
            ya, yb = max(0, ys.start - pad), min(sub.shape[0], ys.stop + pad)
            xa, xb = max(0, xs.start - pad), min(w, xs.stop + pad)
            wgt = np.clip(bg[None, xa:xb] - sub[ya:yb, xa:xb].astype(np.float32), 0, None)
            tot = float(wgt.sum())
            if tot <= 0:
                continue
            cy = float(wgt.sum(1) @ np.arange(ya, yb)) / tot + y0
            cx = float(wgt.sum(0) @ np.arange(xa, xb)) / tot
            blobs.append(dict(u=cx, v=cy, w=bw, h=bh, fill=area / (bw * bh)))
    return blobs


def classify(blobs, pixel_um):
    """按横向宽度分类（横向尺寸只由镜头决定，不受速度影响）。"""
    def width_ok(b, d_um):
        return 0.75 * d_um / pixel_um - 1 <= b["w"] <= 1.3 * d_um / pixel_um + 2

    out = defaultdict(list)
    for b in blobs:
        aspect = b["h"] / b["w"]
        if not 0.3 <= aspect <= 3.5:
            continue
        round_ = 0.62 <= b["fill"] <= 0.88
        if round_ and width_ok(b, 200):
            out["grid"].append(b)
        elif round_ and width_ok(b, 400):
            out["grid_big"].append(b)
        elif b["fill"] > 0.88 and width_ok(b, 400):
            out["square"].append(b)
        elif round_ and width_ok(b, 100):
            out["anchor"].append(b)
    return out


# ---------------------------------------------------------------- 点阵：编号 + 拟合

def lattice_vectors(P):
    """每个点找最近的"横向"邻居和"纵向"邻居，取中位数作两个格向量。不要求横纵间距相近（速度不匹配时纵向会被拉长或压扁）。"""
    d = P[None, :, :] - P[:, None, :]
    dist = np.hypot(d[..., 0], d[..., 1])
    np.fill_diagonal(dist, np.inf)
    horiz = np.abs(d[..., 0]) > np.abs(d[..., 1])
    a_list, b_list = [], []
    for i in range(len(P)):
        for cls, acc in ((horiz[i], a_list), (~horiz[i], b_list)):
            cand = np.where(cls & np.isfinite(dist[i]))[0]
            if len(cand):
                j = cand[np.argmin(dist[i, cand])]
                vec = d[i, j]
                idx = 0 if acc is a_list else 1
                acc.append(vec if vec[idx] > 0 else -vec)
    if len(a_list) < 10 or len(b_list) < 10:
        raise RegisterError("点阵点太少，算不出格距。")
    return np.median(np.array(a_list), axis=0), np.median(np.array(b_list), axis=0)


def index_grid(found, truth):
    """以方块为原点给点阵点编号，换成板坐标。返回 [(u, v, x_mm, y_mm)]、方向信息。"""
    squares = found["square"]
    if len(squares) != 1:
        raise RegisterError(f"应该找到 1 个方向方块，实际 {len(squares)} 个。"
                            "常见原因：B 区不在图里、图太暗或太亮（铬点不够暗）、--pixel-um 差得太多。")
    sq = squares[0]
    dots = found["grid"] + found["grid_big"] + squares
    P = np.array([[b["u"], b["v"]] for b in dots])
    a, b = lattice_vectors(P)
    M = np.column_stack([a, b])
    rel = P - np.array([sq["u"], sq["v"]])
    ij = np.linalg.solve(M, rel.T).T
    IJ = np.round(ij)
    resid = np.hypot(*(rel - IJ @ M.T).T)
    good = resid < 0.25 * min(np.hypot(*a), np.hypot(*b))

    sq_t = next(t for t in truth if t["kind"] == "orient_square")
    grid_t = {(round(t["x"], 3), round(t["y"], 3)): t for t in truth if t["kind"] in ("grid", "orient_square")}
    xs = sorted({k[0] for k in grid_t})
    ys = sorted({k[1] for k in grid_t})
    if not (math.isclose(sq_t["x"], xs[0]) and math.isclose(sq_t["y"], ys[0])):
        raise RegisterError("真值里的方块不在点阵左上角，脚本和真值文件版本不一致。")

    I, J = IJ[good, 0], IJ[good, 1]
    sx = 1 if I.min() >= 0 else -1 if I.max() <= 0 else 0
    sy = 1 if J.min() >= 0 else -1 if J.max() <= 0 else 0
    if not sx or not sy:
        raise RegisterError("点阵编号两边都有，方块位置不对：可能把别的东西认成了方块。")

    pitch_x, pitch_y = xs[1] - xs[0], ys[1] - ys[0]
    out, bad_big = [], 0
    for k in np.where(good)[0]:
        x = round(sq_t["x"] + sx * IJ[k, 0] * pitch_x, 3)
        y = round(sq_t["y"] + sy * IJ[k, 1] * pitch_y, 3)
        t = grid_t.get((x, y))
        if t is None:
            continue
        is_big = dots[k] in found["grid_big"]
        if is_big != (t["size_um"] >= 400 and t["kind"] == "grid"):
            bad_big += 1
        out.append((P[k, 0], P[k, 1], x, y))
    if len(out) < 0.8 * len(grid_t):
        raise RegisterError(f"点阵只编上号 {len(out)} / {len(grid_t)} 个。B 区可能不完整、太暗，或者被裁掉了。")
    n_big = sum(1 for t in grid_t.values() if t["size_um"] >= 400 and t["kind"] == "grid")
    if bad_big > max(2, 0.3 * n_big):
        raise RegisterError("刻度大点的位置和编号对不上，编号可能整体错了一格。")
    orient = dict(mirror_x=bool(sx < 0), reversed_y=bool(sy < 0))
    return np.array(out), orient


class Model:
    """B 区拟合：列 u = 三次多项式(x) + c·y（旋转），行 v = 仿射(x, y)。x 归一化到 [-1, 1] 便于求解。"""

    def __init__(self, G):
        u, v, x, y = G.T
        self.x0, self.xs = 40.0, 40.0
        X = self._xn(x)
        self.Au = np.column_stack([np.ones_like(X), X, X ** 2, X ** 3, y])
        self.cu, *_ = np.linalg.lstsq(self.Au, u, rcond=None)
        Av = np.column_stack([np.ones_like(x), x, y])
        self.cv, *_ = np.linalg.lstsq(Av, v, rcond=None)
        self.res_u = u - self.Au @ self.cu
        self.res_v = v - Av @ self.cv
        self.y_ref = float(np.mean(y))

    def _xn(self, x):
        return (np.asarray(x, dtype=float) - self.x0) / self.xs

    def u(self, x, y):
        X = self._xn(x)
        c = self.cu
        return c[0] + c[1] * X + c[2] * X ** 2 + c[3] * X ** 3 + c[4] * np.asarray(y, dtype=float)

    def v(self, x, y):
        return self.cv[0] + self.cv[1] * np.asarray(x, dtype=float) + self.cv[2] * np.asarray(y, dtype=float)

    def du_dx(self, x):
        """每毫米多少列（横向放大率，含畸变）。"""
        X = self._xn(x)
        c = self.cu
        return (c[1] + 2 * c[2] * X + 3 * c[3] * X ** 2) / self.xs

    @property
    def dv_dy(self):
        return self.cv[2]

    def distortion_um(self, xs):
        """多项式偏离"直线"的最大量（µm）：直线取两端连线。"""
        u = self.u(xs, self.y_ref)
        lin = u[0] + (u[-1] - u[0]) * (xs - xs[0]) / (xs[-1] - xs[0])
        um_per_col = 1000.0 / abs(self.du_dx(40.0))
        return float(np.max(np.abs(u - lin)) * um_per_col)


# ---------------------------------------------------------------- 行标：逐行跟踪

def track_anchor_rows(found, truth, model, grid_y):
    """从点阵最后一行出发，按板上 y 顺序逐行找左右行标，边走边更新"每毫米多少行"。"""
    anchors = np.array([[b["u"], b["v"]] for b in found["anchor"]]) if found["anchor"] else np.zeros((0, 2))
    by_row = defaultdict(list)
    for t in truth:
        if t["kind"] == "anchor":
            by_row[t["y"]].append(t["x"])
    rows_y = sorted(by_row)
    y_start = max(grid_y) if rows_y and rows_y[0] > max(grid_y) else min(grid_y)
    if rows_y and rows_y[0] < y_start:
        rows_y = rows_y[::-1]
    xL, xR = min(min(v) for v in by_row.values()), max(max(v) for v in by_row.values())

    k = model.dv_dy                                   # 每毫米多少行（带符号）
    lat_tol = 1.0 * abs(model.du_dx(40.0))            # 横向允许漂 1 mm
    prev = dict(y=y_start, vL=float(model.v(xL, y_start)), vR=float(model.v(xR, y_start)),
                uL=float(model.u(xL, y_start)), uR=float(model.u(xR, y_start)))
    tilt = model.cv[1]                                # 每毫米 x，行号变多少（板子歪）
    used = np.zeros(len(anchors), bool)
    result = {}
    for y in rows_y:
        dy = y - prev["y"]
        du = model.cu[4] * dy                         # 板子转了一点：沿 y 走，列也跟着挪
        pred = dict(L=(prev["uL"] + du, prev["vL"] + k * dy), R=(prev["uR"] + du, prev["vR"] + k * dy))
        tol = 0.35 * abs(k)
        got = {}
        for side in ("L", "R"):
            if not len(anchors):
                break
            pu, pv = pred[side]
            dist = np.hypot((anchors[:, 0] - pu) / lat_tol, (anchors[:, 1] - pv) / tol)
            dist[used] = np.inf
            j = int(np.argmin(dist))
            if dist[j] <= 1.0:
                got[side] = anchors[j]
                used[j] = True
        if "L" in got and "R" in got:
            (uL, vL), (uR, vR) = got["L"], got["R"]
            status = "both"
        elif got:
            side = next(iter(got))
            us, vs = got[side]
            span_u = model.u(xR, y) - model.u(xL, y)
            if side == "L":
                uL, vL, uR, vR = us, vs, us + span_u, vs + tilt * (xR - xL)
            else:
                uR, vR, uL, vL = us, vs, us - span_u, vs - tilt * (xR - xL)
            status = "one"
        else:
            (uL, vL), (uR, vR) = pred["L"], pred["R"]
            status = "missing"
        if status != "missing" and abs(dy) > 0:
            k_new = ((vL - prev["vL"]) + (vR - prev["vR"])) / 2.0 / dy
            if 0.6 < k_new / k < 1.6:
                k = k_new
        result[y] = dict(uL=float(uL), vL=float(vL), uR=float(uR), vR=float(vR), status=status,
                         xL=xL, xR=xR, k=float(k))
        prev = dict(y=y, uL=uL, vL=vL, uR=uR, vR=vR)
    return result


def fill_missing_rows(rows):
    """一行两个行标都没找到时，用前后找到的行按板上 y 线性插值。"""
    ys = sorted(rows)
    found = [y for y in ys if rows[y]["status"] != "missing"]
    if len(found) < 2:
        return
    for y in ys:
        if rows[y]["status"] != "missing":
            continue
        lo = max([f for f in found if f < y], default=None)
        hi = min([f for f in found if f > y], default=None)
        if lo is None or hi is None:
            lo, hi = (found[0], found[1]) if lo is None else (found[-2], found[-1])
        t = (y - lo) / (hi - lo)
        for key in ("uL", "vL", "uR", "vR"):
            rows[y][key] = rows[lo][key] + t * (rows[hi][key] - rows[lo][key])
        rows[y]["status"] = "interpolated"


def row_mapper(model, row):
    """某一行内：板上 x → 图上 (u, v)。横向用点阵的畸变曲线、两端对上行标；纵向在两个行标之间线性。"""
    xL, xR = row["xL"], row["xR"]
    pL, pR = model.u(xL, model.y_ref), model.u(xR, model.y_ref)

    def f(x):
        x = np.asarray(x, dtype=float)
        s = (model.u(x, model.y_ref) - pL) / (pR - pL)
        u = row["uL"] + s * (row["uR"] - row["uL"])
        v = row["vL"] + (x - xL) / (xR - xL) * (row["vR"] - row["vL"])
        return u, v
    return f


# ---------------------------------------------------------------- 主流程

def register(img, truth, pixel_um=4.88):
    bg = column_background(img)
    if float(np.median(bg)) < 30:
        raise RegisterError(f"背景太暗（中位 {np.median(bg):.0f}）。配准要用明场（背光）图，背景最好在 150–200。")
    blobs = find_blobs(img, bg)
    found = classify(blobs, pixel_um)
    G, orient = index_grid(found, truth)
    model = Model(G)
    rows = track_anchor_rows(found, truth, model, G[:, 3])
    fill_missing_rows(rows)

    mappers = {y: row_mapper(model, r) for y, r in rows.items()}
    out = []
    for t in truth:
        if t["kind"] != "particle":
            continue
        row = rows.get(t["y"])
        if row is None:
            continue
        u, v = mappers[t["y"]](t["x"])
        out.append(dict(x=round(float(u), 3), y=round(float(v), 3), size_um=t["size_um"], id=t["id"],
                        x_mm=t["x"], y_mm=t["y"], row=row["status"]))

    roi = particle_roi(truth, rows, mappers)
    report = make_report(model, orient, rows, found, img.shape, G)
    return out, roi, report


def particle_roi(truth, rows, mappers):
    """每一排颗粒（同一直径的 4 行）一个四边形：横向在行标和颗粒之间，纵向上下各留 0.4 mm（避开直径标注字）。"""
    bands = defaultdict(list)
    for t in truth:
        if t["kind"] == "particle":
            bands[t["size_um"]].append(t)
    polys = []
    for size, pts in sorted(bands.items()):
        ys = sorted({p["y"] for p in pts})
        xs = [p["x"] for p in pts]
        x0, x1 = min(xs) - 0.3, max(xs) + 0.3
        first, last = ys[0], ys[-1]
        if first not in rows or last not in rows:
            continue
        corners = []
        for y, x, ext in ((first, x0, -0.4), (first, x1, -0.4), (last, x1, 0.4), (last, x0, 0.4)):
            u, v = mappers[y](x)
            k = rows[y]["k"]
            corners.append([round(float(u), 1), round(float(v + ext * k), 1)])
        polys.append(dict(size_um=size, polygon=corners))
    return dict(kind="GC-CAL-01 particle bands", polygons=polys)


def make_report(model, orient, rows, found, shape, G):
    um_col_c = 1000.0 / abs(model.du_dx(40.0))
    um_col_l = 1000.0 / abs(model.du_dx(5.0))
    um_col_r = 1000.0 / abs(model.du_dx(75.0))
    ks = [r["k"] for _, r in sorted(rows.items()) if r["status"] == "both"]
    um_line_b = 1000.0 / abs(model.dv_dy)
    um_line = [1000.0 / abs(k) for k in ks] or [um_line_b]
    rot_deg = math.degrees(math.atan2(model.cu[4], abs(model.du_dx(40.0))))
    tilt_deg = math.degrees(math.atan2(model.cv[1], abs(model.du_dx(40.0))))
    status = defaultdict(int)
    for r in rows.values():
        status[r["status"]] += 1
    return dict(
        image=dict(width=int(shape[1]), height=int(shape[0])),
        orientation=dict(chrome_side="朝下（翻面）" if orient["mirror_x"] else "朝上",
                         scan="J 端先进（掉头）" if orient["reversed_y"] else "A 端先进", **orient),
        um_per_px_cross=dict(center=round(um_col_c, 4), left=round(um_col_l, 4), right=round(um_col_r, 4)),
        distortion_um=round(model.distortion_um(np.linspace(2, 78, 77)), 2),
        um_per_line_scan=dict(grid=round(um_line_b, 4), min=round(min(um_line), 4), max=round(max(um_line), 4)),
        aspect_line_over_px=round(um_line_b / um_col_c, 4),
        rotation_deg=round(rot_deg, 4), row_tilt_deg=round(tilt_deg, 4),
        grid_dots=int(len(G)), grid_residual_px=dict(u_rms=round(float(np.sqrt(np.mean(model.res_u ** 2))), 3),
                                                     v_rms=round(float(np.sqrt(np.mean(model.res_v ** 2))), 3)),
        anchor_rows=dict(status),
        found=dict((k, len(v)) for k, v in found.items()),
    )


def summary(rep):
    o, c, s = rep["orientation"], rep["um_per_px_cross"], rep["um_per_line_scan"]
    lines = [
        f"方向：铬面{o['chrome_side']}，{o['scan']}",
        f"横向：中间 {c['center']:.3f} µm/像素（左 {c['left']:.3f}，右 {c['right']:.3f}），畸变最大 {rep['distortion_um']:.1f} µm",
        f"扫描方向：点阵处 {s['grid']:.3f} µm/行，颗粒区 {s['min']:.3f}–{s['max']:.3f} µm/行",
        f"一行长度 ÷ 一像素宽度 = {rep['aspect_line_over_px']:.3f}"
        + ("（正方形像素）" if abs(rep["aspect_line_over_px"] - 1) < 0.02 else
           f"（要正方形像素：速度乘 {1 / rep['aspect_line_over_px']:.3f}，或行频乘 {rep['aspect_line_over_px']:.3f}）"),
        f"旋转 {rep['rotation_deg']:+.3f}°，行倾斜 {rep['row_tilt_deg']:+.3f}°",
        f"点阵 {rep['grid_dots']} 个，偏离直线拟合 列 {rep['grid_residual_px']['u_rms']:.2f} / 行 {rep['grid_residual_px']['v_rms']:.2f} 像素"
        + ("（行方向偏离大：点阵这 10 mm 里玻璃速度就不匀；颗粒按每行的行标定位，不受影响）"
           if rep["grid_residual_px"]["v_rms"] > 0.5 else ""),
        "颗粒行定位：" + "，".join(f"{k} {v}" for k, v in sorted(rep["anchor_rows"].items()))
        + "（both = 两端行标都找到；one = 只找到一端；interpolated = 都没找到，用相邻行插值）",
    ]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scan", type=Path, help="整板扫描图（P5 PGM，明场）")
    ap.add_argument("--truth", type=Path, default=DEFAULT_TRUTH)
    ap.add_argument("--out", type=Path, default=Path("truth_px.csv"), help="像素真值 CSV（给 V4）")
    ap.add_argument("--roi", type=Path, help="颗粒区多边形 JSON（给 V4 --roi）")
    ap.add_argument("--report", type=Path, help="报告 JSON")
    ap.add_argument("--pixel-um", type=float, default=4.88, help="标称物方像素 µm，只用来分类")
    args = ap.parse_args(argv)
    try:
        truth = load_plate_truth(args.truth)
        img = read_pgm(args.scan)
        out, roi, rep = register(img, truth, args.pixel_um)
    except RegisterError as e:
        print(f"配准失败：{e}", file=sys.stderr)
        return 2
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["x", "y", "size_um", "id", "x_mm", "y_mm", "row"])
        w.writeheader()
        w.writerows(out)
    if args.roi:
        args.roi.write_text(json.dumps(roi, ensure_ascii=False, indent=1), encoding="utf-8")
    if args.report:
        args.report.write_text(json.dumps(rep, ensure_ascii=False, indent=1), encoding="utf-8")
    print(summary(rep))
    print(f"写出 {len(out)} 个颗粒的像素真值 → {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
