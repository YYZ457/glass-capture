"""可检性研究：在假设的光学 / 噪声条件下，各种尺寸的颗粒能检出多少、尺寸量得准不准。

把 sim_particles（成像仿真）→ detect_v1（算法原型）→ verify/V4 的配对规则 串起来，
输出 docs/design/detection_plan.md 第 3 节里的那几张表。所有结论都带着"假设"两个字：
等拿到实测噪声和实拍图，改 sim_particles 里的默认值重跑。

用法：python -I host/detect/study_detectability.py [--quick]
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "verify"))
import detect_v1  # noqa: E402
import sim_particles as sim  # noqa: E402
from V4_truth_match import match  # noqa: E402

SIZES = [5, 8, 10, 15, 20, 30, 50]


def run_case(optics, sensor, scene, sizes, count, surface="front", opacity=1.0, seed=1, k=6.0, rel=0.06):
    """返回 {尺寸: dict(recall, d_vol 均值, d_fwhm 均值, edge 均值, 被判成 particle 的比例)} 和误报数。"""
    rng = np.random.default_rng(seed)
    particles = sim.scatter_particles(sizes, count, scene, rng, surface, opacity)
    clean = sim.render([], optics, sensor, scene, rng)
    image = sim.render(particles, optics, sensor, scene, rng)
    bg, sigma = detect_v1.calibrate(clean)
    cands = [c for c in detect_v1.detect(image, bg, sigma, scene.mode, k=k, rel=rel) if c["class"] != "noise"]
    truth = [(p["x"], p["y"], p["size_um"]) for p in particles]
    pairs = match(truth, [(c["cx"], c["cy"]) for c in cands], radius=6.0)
    by_size = {s: {"n": 0, "hit": 0, "d_vol": [], "d_fwhm": [], "edge": [], "particle": 0} for s in sizes}
    for p in particles:
        by_size[p["size_um"]]["n"] += 1
    for ti, ci, _ in pairs:
        row = by_size[truth[ti][2]]
        c = cands[ci]
        row["hit"] += 1
        row["d_fwhm"].append(c["d_fwhm_um"])
        row["edge"].append(c["edge_px"])
        row["particle"] += c["class"] == "particle"
        if "d_vol_um" in c:
            row["d_vol"].append(c["d_vol_um"])
    return by_size, len(cands) - len(pairs), image.size


def fmt(values, digits=1):
    return f"{np.mean(values):.{digits}f} ± {np.std(values):.{digits}f}" if values else "—"


def table(title, by_size, false_alarms, pixels):
    print(f"\n### {title}\n")
    print("| 颗粒直径 µm | 检出 | 检出率 | 体积法直径 µm | 半高法直径 µm | 边缘宽度 px | 判成 particle |")
    print("|---|---|---|---|---|---|---|")
    for size, r in by_size.items():
        rate = r["hit"] / r["n"] if r["n"] else float("nan")
        frac = f"{r['particle'] / r['hit']:.0%}" if r["hit"] else "—"
        print(f"| {size:g} | {r['hit']}/{r['n']} | {rate:.0%} | {fmt(r['d_vol'])} | {fmt(r['d_fwhm'])} | "
              f"{fmt(r['edge'], 2)} | {frac} |")
    print(f"\n误报 {false_alarms} 个 / {pixels / 1e6:.1f} 百万像素")


def gaussian_tail(k):
    return 0.5 * math.erfc(k / math.sqrt(2.0))


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="每种尺寸只撒 20 个（测试用）")
    args = parser.parse_args(argv)
    count = 20 if args.quick else 100

    optics, sensor = sim.OpticsModel(), sim.SensorModel()
    print("## 导出量（由默认假设算出）\n")
    print(f"- 放大倍率 {optics.magnification:.3f}，物方像素 {optics.object_pixel_um:.2f} µm，"
          f"有效光圈 F{optics.f_effective:.1f}，物方数值孔径 {optics.object_na:.4f}")
    print(f"- 镜头弥散 σ = {optics.lens_sigma_px():.2f} px，运动模糊 {optics.motion_px():.2f} px"
          f"（{optics.speed_mm_s:g} mm/s × {optics.exposure_us:g} µs）")
    print(f"- 景深（弥散 ≤ 1 px）±{optics.depth_of_field_um(1.0) / 2:.0f} µm；（≤ 2 px）±{optics.depth_of_field_um(2.0) / 2:.0f} µm")
    for t in (0.5, 0.7, 1.1, 2.0, 3.0):
        o = sim.OpticsModel(glass_thickness_mm=t)
        print(f"- 玻璃厚 {t:g} mm：背面颗粒弥散圆 {o.defocus_blur_px('back'):.1f} px")
    print(f"- 噪声：灰度 170 时 {sensor.noise_dn(170):.2f} DN，灰度 6 时 {sensor.noise_dn(6):.2f} DN")

    scene = sim.SceneModel(mode="bright", background_dn=170.0)
    for opacity, name in ((1.0, "明场，正面，不透光颗粒（对比度 100%）"),
                          (0.4, "明场，正面，半透明颗粒（等效挡光 40%）"),
                          (0.15, "明场，正面，很透明的颗粒（等效挡光 15%）")):
        table(name, *run_case(optics, sensor, scene, SIZES, count, opacity=opacity))
    table("明场，背面（玻璃厚 0.7 mm），不透光颗粒", *run_case(optics, sensor, scene, SIZES, count, surface="back"))

    dim = sim.SceneModel(mode="bright", background_dn=40.0)
    table("明场，光不够（背景只有 40），不透光颗粒", *run_case(optics, sensor, dim, SIZES, count))

    dark = sim.SceneModel(mode="dark", background_dn=6.0, dark_signal_dn=120.0)
    table("暗场，正面（完全分辨的颗粒亮度 120、背景 6）", *run_case(optics, sensor, dark, SIZES, count, k=6.0, rel=0.0))

    print("\n## 纯噪声误报（高斯近似，246 百万像素 / 秒）\n")
    print("| 阈值 k | 单像素超阈概率 | 单像素候选 / 秒 | 面积 ≥ 2 的候选 / 小时 |")
    print("|---|---|---|---|")
    for k in (4.0, 5.0, 6.0, 7.0):
        p = gaussian_tail(k)                 # 单方向
        print(f"| {k:g} | {p:.1e} | {p * 246e6:.2g} | {4 * p * p * 246e6 * 3600:.2g} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
