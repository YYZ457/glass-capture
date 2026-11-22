"""生成标定玻璃板 GC-CAL-01 的版图：给厂家的 GDS / DXF（zip）、真值坐标、网页示意图。

一份代码出所有文件，保证"厂家做出来的"和"程序认为在哪"是同一套坐标。

坐标（板坐标，单位 mm，铬面朝上、从相机这边看）：
  x：横跨视场（相机一行 16384 像素的方向），0..80
  y：沿扫描方向（玻璃移动方向），0..105，y=0 是先进入视场的一端
  翻面、掉头扫都不用手改坐标：verify/plate_register.py 会自己认出方向。

区段：A 空白、S 静态调焦、B 点阵尺、E 颗粒点阵（每行两端有行标）、J 黑条、T 手持区。
设计理由和精度推理见 docs/design/calib_target/README.md。

用法：
  pip install gdstk ezdxf
  python tools/calib_target.py                       # → docs/design/calib_target/
  python tools/calib_target.py --out DIR --svg-only  # 只出示意图和真值，不需要 gdstk / ezdxf
"""
import argparse
import csv
import json
import math
from pathlib import Path

NAME = "GC-CAL-01"
VERSION = "v1"
W, L = 80.0, 105.0          # 板宽（横跨视场）、板长（扫描方向），mm
EDGE = 1.5                  # 四周留白：厂家夹持、切割崩边
PIX_UM = 4.88               # 物方像素（detection_plan 2.1）
SEG = 10.0                  # 中控台"8 段锐度"每段在板上的宽度（16384 列 ÷ 8 × 4.88 µm）
FOCUS_LPMM = 40             # 静态调焦细线的频率

# 区段：(代号, 名称, y0, y1)。网页和真值文件都用这张表。
ZONES = [
    ("A", "引导空白", 0.0, 10.0),
    ("S", "静态调焦", 11.0, 29.0),
    ("B", "点阵尺", 31.0, 43.0),
    ("E", "颗粒点阵", 45.0, 85.0),
    ("J", "黑条与板号", 87.0, 94.0),
    ("T", "手持区", 95.0, 105.0),
]

PARTICLE_SIZES = [2, 5, 8, 10, 15, 20, 30, 50]  # µm
PARTICLE_ROWS, PARTICLE_COLS = 4, 76            # 每种 304 个
ANCHOR_X = (1.9, 78.1)                          # 颗粒区每行两端的行标，x mm
ANCHOR_UM = 100                                 # 行标直径


def zone(code):
    return next(z for z in ZONES if z[0] == code)


class Layout:
    """收集图形。圆存圆心半径（GDS 再离散），其余存多边形顶点（mm）。"""

    def __init__(self):
        self.circles = []    # (x, y, r, zone)
        self.polys = []      # ([(x, y)...], zone)
        self.texts = []      # (text, x, y, height, zone)
        self.truth = []      # dict 行

    def circle(self, x, y, d_um, zone_code, kind="dot", note=""):
        self.circles.append((x, y, d_um / 2000.0, zone_code))
        self.truth.append(dict(zone=zone_code, kind=kind, x_mm=round(x, 6), y_mm=round(y, 6),
                               size_um=d_um, length_mm="", note=note))

    def poly(self, pts, zone_code):
        self.polys.append(([(round(px, 6), round(py, 6)) for px, py in pts], zone_code))

    def rect(self, x0, y0, x1, y1, zone_code):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], zone_code)

    def text(self, s, x, y, h, zone_code):
        self.texts.append((s, x, y, h, zone_code))


def build():
    lay = Layout()

    # S 静态调焦：每 10 mm（中控台一段）一个相同单元，图案全部平行于长边。
    #   1 mm 宽条：两条干净的边，给"边缘宽度"（两侧要各 48 px ≈ 0.23 mm 的平台）
    #   3 mm 宽 40 lp/mm 细线：给"锐度"，这个频率对离焦最敏感又不贴像素极限
    y0, y1 = zone("S")[2], zone("S")[3]
    w = 1.0 / FOCUS_LPMM / 2
    for k in range(8):
        cx = SEG * k + SEG / 2
        lay.rect(cx - 2.5, y0, cx - 1.5, y1, "S")
        lay.truth.append(dict(zone="S", kind="focus_bar", x_mm=cx - 2.0, y_mm=(y0 + y1) / 2, size_um=1000,
                              length_mm=y1 - y0, note=f"第 {k + 1} 段"))
        n = int(round(3.0 / (2 * w)))
        for i in range(n):
            x = cx - 0.5 + 2 * i * w
            lay.rect(x, y0, x + w, y1, "S")
        lay.truth.append(dict(zone="S", kind="focus_lines", x_mm=cx + 1.0, y_mm=(y0 + y1) / 2,
                              size_um=round(w * 1000, 2), length_mm=y1 - y0,
                              note=f"第 {k + 1} 段，{FOCUS_LPMM} lp/mm，{n} 根"))

    # B 点阵尺：1 mm 间距、Ø0.2 mm；两端行每 10 mm 一个 Ø0.4 mm 大点当刻度；左上角换成方块认方向
    yb = int(zone("B")[2])
    for y in range(yb + 1, yb + 12):
        for x in range(2, 79):
            if x == 2 and y == yb + 1:
                lay.rect(1.8, y - 0.2, 2.2, y + 0.2, "B")
                lay.truth.append(dict(zone="B", kind="orient_square", x_mm=2.0, y_mm=float(y), size_um=400,
                                      length_mm="", note="方向标记：铬面朝上、y 从小到大时在左上"))
                continue
            big = x % 10 == 0 and y in (yb + 1, yb + 11)
            lay.circle(float(x), float(y), 400 if big else 200, "B", kind="grid",
                       note="10 mm 刻度点" if big else "")

    # E 颗粒点阵：8 种直径，每种 4 行 × 76 列 = 304 个，间距 1 mm（= 204.9 px，不是整数，像素相位自然铺开）
    ye = zone("E")[2]
    for k, d in enumerate(PARTICLE_SIZES):
        y0 = ye + 5.0 * k
        lay.text(f"{d}um", 2.2, y0 + 0.15, 0.35, "E")
        for r in range(PARTICLE_ROWS):
            y = y0 + 1.0 + r
            for c in range(PARTICLE_COLS):
                lay.circle(2.5 + c, y, d, "E", kind="particle")
            # 行标：每行两端各一个 Ø0.1 mm。玻璃速度不匀时，离点阵尺几十毫米的行会偏出几百像素，
            # 配准靠它们逐行定位（verify/plate_register.py）。宽 20 px，和颗粒（≤ 10 px）、点阵（41 px）都分得开。
            for x in ANCHOR_X:
                lay.circle(x, y, ANCHOR_UM, "E", kind="anchor", note=f"{d} um 这一排的行标")

    # J 黑条（全铬，3 mm）+ 板号
    yj = zone("J")[2]
    lay.rect(2.0, yj + 1.0, 78.0, yj + 4.0, "J")
    lay.truth.append(dict(zone="J", kind="black_bar", x_mm=40.0, y_mm=yj + 2.5, size_um=3000, length_mm=76.0,
                          note="每列不透光参考；上沿是一条全宽横边"))
    lay.text(f"{NAME} {VERSION}  1.0mm", 4.0, yj + 5.2, 1.2, "J")
    return lay


# ---------------------------------------------------------------- 输出

def circle_points(x, y, r):
    """圆 → 正多边形（mm）。顶点半径按"面积相等"放大，避免内接多边形让点系统性偏小
    （23 边内接时直径小 0.6%）。顶点间距约 0.1 µm 起，和厂家写入网格同量级。"""
    d_um = 2 * r * 1000
    n = 32 if d_um < 20 else 64 if d_um < 100 else 128
    rc = r * math.sqrt(2 * math.pi / (n * math.sin(2 * math.pi / n)))
    return [(x + rc * math.cos(2 * math.pi * i / n), y + rc * math.sin(2 * math.pi * i / n)) for i in range(n)]


def write_gds(lay, path):
    import gdstk
    lib = gdstk.Library(name=NAME, unit=1e-6, precision=1e-9)     # 用户单位 µm，精度 1 nm
    cell = lib.new_cell(NAME)
    for x, y, r, _ in lay.circles:
        cell.add(gdstk.Polygon([(px * 1000, py * 1000) for px, py in circle_points(x, y, r)], layer=1))
    for pts, _ in lay.polys:
        cell.add(gdstk.Polygon([(px * 1000, py * 1000) for px, py in pts], layer=1))
    for s, x, y, h, _ in lay.texts:
        for p in gdstk.text(s, h * 1000, (x * 1000, y * 1000), layer=1):
            cell.add(p)
    cell.add(gdstk.rectangle((0, 0), (W * 1000, L * 1000), layer=2))   # 外形，只作参考
    lib.write_gds(str(path))


def write_dxf(lay, path):
    import io
    import zipfile

    import ezdxf
    import gdstk
    doc = ezdxf.new("R2010", setup=False)
    doc.units = ezdxf.units.MM
    doc.layers.add("CHROME", color=7)
    doc.layers.add("OUTLINE", color=1)
    msp = doc.modelspace()
    for x, y, r, _ in lay.circles:
        msp.add_lwpolyline(circle_points(x, y, r), close=True, dxfattribs={"layer": "CHROME"})
    for pts, _ in lay.polys:
        msp.add_lwpolyline(pts, close=True, dxfattribs={"layer": "CHROME"})
    for s, x, y, h, _ in lay.texts:
        for p in gdstk.text(s, h, (x, y)):
            msp.add_lwpolyline([tuple(v) for v in p.points], close=True, dxfattribs={"layer": "CHROME"})
    msp.add_lwpolyline([(0, 0), (W, 0), (W, L), (0, L)], close=True, dxfattribs={"layer": "OUTLINE"})
    buf = io.StringIO()                         # 原始 DXF 体积大，打包成 zip 进仓库
    doc.write(buf)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(f"{NAME}.dxf", buf.getvalue())


def write_truth(lay, out):
    fields = ["id", "zone", "kind", "x_mm", "y_mm", "size_um", "length_mm", "note"]
    with open(out / f"{NAME}_truth_mm.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for i, row in enumerate(lay.truth):
            w.writerow(dict(id=i, **row))
    meta = dict(
        name=NAME, version=VERSION, width_mm=W, length_mm=L, thickness_mm=1.0, edge_clear_mm=EDGE,
        focus_segment_mm=SEG, focus_lp_per_mm=FOCUS_LPMM,
        coordinates="板坐标 mm；铬面朝上；x 横跨视场，y 沿扫描方向；铬面朝下时 x -> 80 - x",
        zones=[dict(code=c, name=n, y0_mm=a, y1_mm=b) for c, n, a, b in ZONES],
        particle_sizes_um=PARTICLE_SIZES, particles_per_size=PARTICLE_ROWS * PARTICLE_COLS,
        counts={k: sum(1 for t in lay.truth if t["kind"] == k)
                for k in sorted({t["kind"] for t in lay.truth})},
    )
    (out / f"{NAME}_zones.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    return meta


# ---- 示意图（SVG，网页内嵌；颜色用 CSS 类，跟主题走）。方向和相机图像一致：x 横向，y 向下。

def _svg_shapes(lay, x0, y0, x1, y1, s, min_r_px=0.0):
    out = []
    for x, y, r, _ in lay.circles:
        if x0 - r <= x <= x1 + r and y0 - r <= y <= y1 + r:
            out.append(f'<circle cx="{(x - x0) * s:.2f}" cy="{(y - y0) * s:.2f}" r="{max(r * s, min_r_px):.2f}"/>')
    for pts, _ in lay.polys:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        if max(xs) < x0 or min(xs) > x1 or max(ys) < y0 or min(ys) > y1:
            continue
        d = " ".join(f"{(px - x0) * s:.2f},{(py - y0) * s:.2f}" for px, py in pts)
        out.append(f'<polygon points="{d}"/>')
    return out


def _svg(w, h, cls, label, body):
    return (f'<svg viewBox="0 0 {w:.0f} {h:.0f}" xmlns="http://www.w3.org/2000/svg" class="{cls}" '
            f'role="img" aria-label="{label}"><rect class="glass" x="0" y="0" width="{w:.0f}" height="{h:.0f}"/>'
            f'{body}</svg>')


def write_svgs(lay, out):
    for old in out.glob("detail_*.svg"):
        old.unlink()
    s = 7.0                                      # 整板 7 px/mm → 560 × 735
    body = f'<g class="chrome">{"".join(_svg_shapes(lay, 0, 0, W, L, s, min_r_px=0.3))}</g>'
    (out / "overview.svg").write_text(_svg(W * s, L * s, "plate", f"{NAME} 整板版图", body), encoding="utf-8")

    for code, (a, b, c, d, sc) in {
        "S": (0.0, 15.0, 20.0, 19.0, 50),      # 静态调焦前两段
        "B": (1.0, 31.0, 13.0, 35.0, 60),      # 点阵尺左上角（方向块 + 刻度点）
    }.items():
        body = f'<g class="chrome">{"".join(_svg_shapes(lay, a, b, c, d, sc))}</g>'
        (out / f"detail_{code}.svg").write_text(
            _svg((c - a) * sc, (d - b) * sc, "detail", f"区段 {code} 局部", body), encoding="utf-8")

    # E：按真实尺寸放大到 1.6 px/µm，叠 4.88 µm 像素网格，看每种颗粒盖住几个像素
    k = 1.6
    wpx, hpx = len(PARTICLE_SIZES) * 100 * k, 120 * k
    step = PIX_UM * k
    grid = [f'<line x1="{i * step:.2f}" y1="0" x2="{i * step:.2f}" y2="{hpx}"/>' for i in range(int(wpx / step) + 1)]
    grid += [f'<line x1="0" y1="{j * step:.2f}" x2="{wpx}" y2="{j * step:.2f}"/>' for j in range(int(hpx / step) + 1)]
    items = []
    for i, d in enumerate(PARTICLE_SIZES):
        cx = (50 + 100 * i) * k
        items.append(f'<circle class="dot" cx="{cx:.1f}" cy="{58 * k:.1f}" r="{d / 2 * k:.2f}"/>')
        items.append(f'<text x="{cx:.1f}" y="{112 * k:.1f}">{d} µm</text>')
    body = f'<g class="pix">{"".join(grid)}</g>{"".join(items)}'
    (out / "detail_E.svg").write_text(_svg(wpx, hpx, "detail ladder", "七种颗粒直径与像素网格", body),
                                      encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "docs/design/calib_target"))
    ap.add_argument("--svg-only", action="store_true")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    lay = build()
    write_svgs(lay, out)
    meta = write_truth(lay, out)
    if not args.svg_only:
        write_gds(lay, out / f"{NAME}.gds")
        write_dxf(lay, out / f"{NAME}_dxf.zip")
    print(json.dumps(meta["counts"], ensure_ascii=False))


if __name__ == "__main__":
    main()
