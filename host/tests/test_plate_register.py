"""verify/plate_register.py：用版图画一张"拍歪了"的模拟扫描图，看配准能不能把每个颗粒找准。

模拟图按 8 µm/像素画（真机 4.88 µm），整板约 10000 × 13000，几秒画完。故意加进去的毛病：
桶形畸变、旋转、行倾斜、玻璃速度 ±5% 正弦起伏、横向漂移、噪声；另一张再翻面 + 掉头。
"""
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "verify"))
PIL = pytest.importorskip("PIL")
pytest.importorskip("scipy")
from PIL import Image, ImageDraw  # noqa: E402

import calib_target as ct  # noqa: E402
import plate_register as pr  # noqa: E402

PX_UM = 8.0


class Scan:
    """板坐标 (mm) → 图像像素（像素中心是整数，和质心约定一致）。"""

    def __init__(self, mirror=False, reverse=False, px_um=PX_UM, line_um=None, speed_ripple=0.05,
                 barrel=0.002, rot=0.004, tilt=-0.004, drift_px=12.0, seed=1):
        self.mirror, self.reverse, self.px = mirror, reverse, px_um
        self.barrel, self.rot, self.tilt, self.drift = barrel, rot, tilt, drift_px
        line0 = line_um or px_um * 1.07                  # 速度没调准：一行比一像素长 7%
        ys = np.linspace(-1, ct.L + 1, 20001)
        lines = 1000.0 / (line0 * (1 + speed_ripple * np.sin(2 * np.pi * ys / 37.0)))
        self._ys = ys
        self._Y = np.concatenate([[0], np.cumsum((lines[1:] + lines[:-1]) / 2 * np.diff(ys))])
        self.u0, self.v0 = 60.0, 40.0
        self.width = int(ct.W * 1000 / px_um * (1 + barrel) + 2 * self.u0 + 40)
        self.height = int(self._Y[-1] + 2 * self.v0)
        self.rng = np.random.default_rng(seed)

    def Y(self, y):
        y = np.asarray(y, dtype=float)
        v = np.interp(y, self._ys, self._Y)
        return (self._Y[-1] - v) if self.reverse else v

    def map(self, x, y):
        x = np.asarray(x, dtype=float)
        X = (ct.W - x) if self.mirror else x
        s = X * 1000 / self.px * (1 + self.barrel * ((X - ct.W / 2) / (ct.W / 2)) ** 2)
        Yp = self.Y(y)
        u = self.u0 + s + self.rot * Yp + self.drift * np.sin(2 * np.pi * np.asarray(y, float) / 61.0)
        v = self.v0 + Yp + self.tilt * s
        return u, v

    def render(self, bg=180, chrome=8, noise=2.0, with_text=True):
        lay = ct.build()
        im = Image.new("L", (self.width, self.height), bg)
        d = ImageDraw.Draw(im)

        def poly(pts):
            xs, ys = zip(*pts)
            u, v = self.map(xs, ys)
            d.polygon([(a + 0.5, b + 0.5) for a, b in zip(u, v)], fill=chrome)   # PIL 的像素中心在 +0.5

        for x, y, r, _ in lay.circles:
            poly(ct.circle_points(x, y, r))
        for pts, _ in lay.polys:
            poly(pts)
        if with_text:
            try:
                import gdstk
                for s, x, y, h, _ in lay.texts:
                    for p in gdstk.text(s, h, (x, y)):
                        poly([tuple(q) for q in p.points])
            except ImportError:
                pass
        img = np.array(im, dtype=np.uint8)
        for r0 in range(0, img.shape[0], 1024):             # 分块加噪声，整板真尺寸也不占几 GB
            blk = img[r0:r0 + 1024].astype(np.float32)
            blk += self.rng.standard_normal(blk.shape, dtype=np.float32) * noise
            img[r0:r0 + 1024] = np.clip(blk, 0, 255)
        return img


def run_case(scan, tol_px=0.35):
    truth = pr.load_plate_truth(REPO / "docs/design/calib_target/GC-CAL-01_truth_mm.csv")
    img = scan.render()
    out, roi, rep = pr.register(img, truth, pixel_um=PX_UM)
    assert len(out) == sum(1 for t in truth if t["kind"] == "particle")
    u_true, v_true = scan.map([o["x_mm"] for o in out], [o["y_mm"] for o in out])
    err = np.hypot(np.array([o["x"] for o in out]) - u_true, np.array([o["y"] for o in out]) - v_true)
    assert err.max() < tol_px, (err.max(), rep)
    return out, roi, rep, err


def test_registers_a_skewed_scan_with_uneven_speed():
    out, roi, rep, err = run_case(Scan())
    assert rep["orientation"]["mirror_x"] is False and rep["orientation"]["reversed_y"] is False
    assert rep["anchor_rows"] == {"both": 32}
    assert abs(rep["um_per_px_cross"]["center"] - PX_UM / (1 - 0.002 * 0)) < 0.1
    assert 1.03 < rep["aspect_line_over_px"] < 1.12           # 设的是 1.07 ± 速度起伏
    assert len(roi["polygons"]) == len(ct.PARTICLE_SIZES)
    # 每个颗粒都落在自己那一排的多边形里，别的排的多边形里没有它
    for o in out[::37]:
        hits = [p["size_um"] for p in roi["polygons"] if pr_inside(p["polygon"], o["x"], o["y"])]
        assert hits == [o["size_um"]]


def test_registers_flipped_and_reversed_plate():
    out, roi, rep, err = run_case(Scan(mirror=True, reverse=True, rot=-0.003, tilt=0.005, seed=2))
    assert rep["orientation"]["mirror_x"] is True and rep["orientation"]["reversed_y"] is True
    assert rep["anchor_rows"] == {"both": 32}


def test_report_without_anchor_rows_falls_back_to_interpolation():
    """行标全看不见（比如被挡住）时不报错，按点阵外推，状态写清楚。速度起伏设小，外推才准。"""
    scan = Scan(speed_ripple=0.0, drift_px=0.0, seed=3)
    truth = [t for t in pr.load_plate_truth(REPO / "docs/design/calib_target/GC-CAL-01_truth_mm.csv")]
    img = scan.render()
    # 把行标涂成背景
    for t in truth:
        if t["kind"] == "anchor":
            u, v = scan.map(t["x"], t["y"])
            r = int(0.08 * 1000 / PX_UM)
            img[int(v) - r:int(v) + r + 1, int(u) - r:int(u) + r + 1] = 180
    out, roi, rep = pr.register(img, truth, pixel_um=PX_UM)
    assert rep["anchor_rows"] == {"missing": 32} or set(rep["anchor_rows"]) <= {"missing", "interpolated"}
    u_true, v_true = scan.map([o["x_mm"] for o in out], [o["y_mm"] for o in out])
    err = np.hypot(np.array([o["x"] for o in out]) - u_true, np.array([o["y"] for o in out]) - v_true)
    assert err.max() < 1.0


def test_fails_clearly_on_a_dark_image():
    truth = pr.load_plate_truth(REPO / "docs/design/calib_target/GC-CAL-01_truth_mm.csv")
    with pytest.raises(pr.RegisterError, match="背景太暗"):
        pr.register(np.full((400, 400), 6, np.uint8), truth)


def test_cli_and_v4_roi(tmp_path):
    scan = Scan(seed=4)
    img = scan.render()
    pgm = tmp_path / "scan.pgm"
    with open(pgm, "wb") as f:
        f.write(f"P5\n{img.shape[1]} {img.shape[0]}\n255\n".encode())
        f.write(img.tobytes())
    r = subprocess.run([sys.executable, "-I", str(REPO / "verify/plate_register.py"), str(pgm),
                        "--out", str(tmp_path / "t.csv"), "--roi", str(tmp_path / "roi.json"),
                        "--report", str(tmp_path / "rep.json"), "--pixel-um", str(PX_UM)],
                       capture_output=True, text=True, check=True)
    assert "方向：铬面朝上" in r.stdout and "颗粒行定位：both 32" in r.stdout
    # 假候选：所有 ≥ 15 µm 的真颗粒 + 每个点阵点（区外，不该算误报）
    import csv
    import json
    rows = list(csv.DictReader(open(tmp_path / "t.csv", encoding="utf-8")))
    cands = [dict(cx=float(t["x"]) + 0.3, cy=float(t["y"]) - 0.2) for t in rows if float(t["size_um"]) >= 15]
    truth = pr.load_plate_truth(REPO / "docs/design/calib_target/GC-CAL-01_truth_mm.csv")
    for t in truth:
        if t["kind"] == "grid":
            u, v = scan.map(t["x"], t["y"])
            cands.append(dict(cx=float(u), cy=float(v)))
    (tmp_path / "c.json").write_text(json.dumps({"candidates": cands}))
    r = subprocess.run([sys.executable, "-I", str(REPO / "verify/V4_truth_match.py"), str(tmp_path / "c.json"),
                        str(tmp_path / "t.csv"), "--roi", str(tmp_path / "roi.json")],
                       capture_output=True, text=True, check=True)
    assert "false alarms 0" in r.stdout
    assert "15 um  304/304" in r.stdout and "10 um  0/304" in r.stdout


def pr_inside(poly, x, y):
    sys.path.insert(0, str(REPO / "verify"))
    import V4_truth_match
    return V4_truth_match.inside([tuple(p) for p in poly], x, y)
