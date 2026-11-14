"""颗粒成像的物理仿真：给定光学、传感器、颗粒参数，生成一张 Mono8 线扫图 + 真值表。

为什么要有它：真值样片还没做、板子暂时碰不到。检测方案（docs/design/detection_plan.md）里
"多大的颗粒、多少对比度能检出"这些数字，就是用它在假设条件下算出来的。等有了实拍图和
实测噪声（光子转移曲线），把 SensorModel / OpticsModel 的默认值换成实测值再跑一遍即可。

模型（每一项对应方案第 2、3 节的一个公式）：
  1. 颗粒 = 物面上的圆盘（直径 µm）。明场：挡光，透过率降到 1 - opacity；
     暗场：散射光，亮度 = dark_signal_dn × 覆盖率。
  2. 模糊 = 镜头（衍射 + 像差，按高斯近似）⊗ 离焦圆盘（背面颗粒 / 高度误差）
           ⊗ 像元开口（1 × 1 像素方框）⊗ 运动（扫描方向，速度 × 曝光时间）。
  3. 噪声：方差(e-) = 读出噪声² + 信号电子数（散粒噪声）；逐列增益不均（PRNU）、逐列暗偏置（DSNU）；
     光照沿宽度方向不均匀（边缘变暗）；最后量化成 8 位。
坐标：像素中心是整数点，和 sg_candidates 的质心约定一致。真值 CSV 表头 x,y,size_um（verify/V4 直接能读），
     另加 surface、opacity 两列。

用法：
  python -I host/detect/sim_particles.py OUT_DIR --sizes 30,20,10 --count 200 [--mode bright|dark]
  → OUT_DIR/scene.pgm、clean.pgm（无颗粒，用来标定背景）、truth.csv、params.json
"""
import argparse
import json
import math
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

SUPERSAMPLE = 8          # 每个像素细分成 8 × 8 来画颗粒


@dataclass
class OpticsModel:
    pixel_um: float = 3.5            # 像元 [厂家彩页]
    fov_mm: float = 80.0             # 视场 [D002]
    sensor_pixels: int = 16384
    f_number: float = 5.6            # [假设] 镜头光圈，待负责人提供
    wavelength_um: float = 0.55      # [假设] 白光 / 绿光
    lens_quality: float = 1.4        # [假设] 实际镜头弥散 ≈ 衍射极限的 1.4 倍
    speed_mm_s: float = 75.0         # [D002]
    exposure_us: float = 50.0
    glass_thickness_mm: float = 0.7  # [假设] 待负责人提供
    glass_index: float = 1.52
    focus_error_um: float = 0.0      # 正面离焦（传送高度误差）

    @property
    def magnification(self):
        return self.pixel_um * self.sensor_pixels / (self.fov_mm * 1000.0)

    @property
    def object_pixel_um(self):
        return self.fov_mm * 1000.0 / self.sensor_pixels

    @property
    def f_effective(self):
        return self.f_number * (1.0 + self.magnification)

    @property
    def object_na(self):
        return self.magnification / (2.0 * self.f_effective)

    def lens_sigma_px(self):
        """镜头点扩散函数的高斯 σ（像素）。衍射艾里斑核心的高斯近似 σ ≈ 0.42 λ N_eff。"""
        return 0.42 * self.wavelength_um * self.f_effective * self.lens_quality / self.pixel_um

    def defocus_blur_px(self, surface):
        """离焦弥散圆直径（像素）。背面颗粒的等效离焦 = 厚度 / 折射率。"""
        dz = abs(self.focus_error_um)
        if surface == "back":
            dz = abs(self.focus_error_um + self.glass_thickness_mm * 1000.0 / self.glass_index)
        return dz * 2.0 * self.object_na / self.object_pixel_um

    def depth_of_field_um(self, blur_px=1.0):
        """弥散圆不超过 blur_px 个像素的总景深（物方，µm）。"""
        return blur_px * self.object_pixel_um / self.object_na

    def motion_px(self):
        return self.speed_mm_s * 1000.0 * self.exposure_us * 1e-6 / self.object_pixel_um


@dataclass
class SensorModel:
    electrons_per_dn: float = 40.0   # [假设] 满阱约 1 万电子 ÷ 255；用光子转移曲线实测后替换
    read_noise_e: float = 13.0       # [假设] 由彩页动态范围 58 dB 反推：满阱 / 794
    black_level_dn: float = 2.0
    prnu: float = 0.01               # 逐列增益不均（1 σ）
    dsnu_dn: float = 0.3             # 逐列暗偏置不均（1 σ）
    gain: float = 1.0                # 模拟增益（倍）
    fpn_seed: int = 2026             # 固定图案（PRNU / DSNU）是传感器的固有属性：同一个种子 = 同一台相机

    def noise_dn(self, level_dn):
        """某个灰度下的时间噪声（DN，1 σ），不含量化。"""
        k = self.electrons_per_dn / self.gain
        signal_e = max(level_dn - self.black_level_dn, 0.0) * k
        return math.sqrt(self.read_noise_e ** 2 + signal_e) / k


@dataclass
class SceneModel:
    mode: str = "bright"             # bright：背景亮、颗粒暗；dark：背景暗、颗粒亮
    background_dn: float = 170.0     # 明场背景目标灰度；暗场时是杂散光背景
    dark_signal_dn: float = 120.0    # 暗场：完全分辨的颗粒的亮度（取决于光源，必须实测）
    edge_falloff: float = 0.25       # 视场边缘比中心暗多少（光源 + 镜头渐晕）
    width: int = 4096
    height: int = 1000


def _fft_kernel(shape, kernel):
    """把一个小核放到大小为 shape 的数组中心对齐到原点，返回它的 FFT。"""
    big = np.zeros(shape)
    kh, kw = kernel.shape
    big[:kh, :kw] = kernel
    big = np.roll(big, (-(kh // 2), -(kw // 2)), axis=(0, 1))
    return np.fft.rfft2(big)


def particle_patch(diameter_um, sub_x, sub_y, surface, optics):
    """一个颗粒在像素网格上的"覆盖率"小图（0..1，已经过全部模糊）。

    sub_x / sub_y：颗粒中心相对于小图中心像素的偏移（-0.5..0.5 像素）。
    返回 (patch, half)：patch 是 (2*half+1)² 的数组，中心像素在 [half, half]。
    """
    s = SUPERSAMPLE
    px = optics.object_pixel_um
    r_px = diameter_um / 2.0 / px
    sigma = optics.lens_sigma_px()
    blur = optics.defocus_blur_px(surface)
    motion = optics.motion_px()
    half = int(math.ceil(r_px + 4.0 * sigma + blur / 2.0 + motion + 2.0))
    n = (2 * half + 1) * s
    # 细分网格上每个小格的中心坐标（单位：像素，原点在中心像素的中心）
    c = (np.arange(n) + 0.5) / s - (half + 0.5)
    xx, yy = np.meshgrid(c, c)
    disc = ((xx - sub_x) ** 2 + (yy - sub_y) ** 2 <= r_px ** 2).astype(np.float64)
    if disc.sum() == 0:                      # 比一个小格还小：按面积放进最近的小格
        iy = int(np.argmin(np.abs(c - sub_y)))
        ix = int(np.argmin(np.abs(c - sub_x)))
        disc[iy, ix] = math.pi * r_px ** 2 * s * s
    spec = np.fft.rfft2(disc)

    fy = np.fft.fftfreq(n, d=1.0 / s)[:, None]       # 周期 / 像素
    fx = np.fft.rfftfreq(n, d=1.0 / s)[None, :]
    spec *= np.exp(-2.0 * (math.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))      # 镜头
    spec *= np.sinc(fx) * np.sinc(fy)                                        # 像元开口
    if motion > 0:
        spec *= np.sinc(fy * motion)                                         # 运动（扫描方向 = y）
    if blur * s >= 1.5:                                                      # 离焦圆盘
        kr = blur * s / 2.0
        k = int(math.ceil(kr)) * 2 + 1
        kc = np.arange(k) - k // 2
        kern = (kc[None, :] ** 2 + kc[:, None] ** 2 <= kr ** 2).astype(np.float64)
        spec *= _fft_kernel((n, n), kern / kern.sum())
    img = np.fft.irfft2(spec, s=(n, n))
    patch = img[s // 2::s, s // 2::s]                 # 取每个像素中心处的值
    return np.clip(patch, 0.0, None), half


def illumination(width, falloff):
    """沿宽度方向的光照：中间 1.0，两边降到 1 - falloff，外加 2% 的缓慢起伏。"""
    u = np.linspace(-1.0, 1.0, width)
    return (1.0 - falloff * u ** 2) * (1.0 + 0.02 * np.sin(u * 9.0))


def render(particles, optics, sensor, scene, rng):
    """particles：dict 列表，键 x、y（像素）、size_um、surface（front/back）、opacity（0..1）。

    返回 uint8 图像（height × width）。particles 为空就是干净背景（标定用）。
    """
    h, w = scene.height, scene.width
    cover = np.zeros((h, w))
    for p in particles:
        ix, iy = int(round(p["x"])), int(round(p["y"]))
        patch, half = particle_patch(p["size_um"], p["x"] - ix, p["y"] - iy, p["surface"], optics)
        y0, y1, x0, x1 = iy - half, iy + half + 1, ix - half, ix + half + 1
        cy0, cx0 = max(y0, 0), max(x0, 0)
        cy1, cx1 = min(y1, h), min(x1, w)
        if cy0 >= cy1 or cx0 >= cx1:
            continue
        sub = patch[cy0 - y0:cy1 - y0, cx0 - x0:cx1 - x0] * p.get("opacity", 1.0)
        cover[cy0:cy1, cx0:cx1] = np.maximum(cover[cy0:cy1, cx0:cx1], sub)

    light = illumination(w, scene.edge_falloff)[None, :]
    k = sensor.electrons_per_dn / sensor.gain
    if scene.mode == "bright":
        ideal_dn = scene.background_dn * light * (1.0 - np.clip(cover, 0.0, 1.0))
    elif scene.mode == "dark":
        ideal_dn = scene.background_dn * light + scene.dark_signal_dn * light * cover
    else:
        raise ValueError("mode must be bright or dark")
    fixed = np.random.default_rng(sensor.fpn_seed)       # 每次渲染都一样，干净图和带颗粒的图才对得上
    col_gain = 1.0 + sensor.prnu * fixed.standard_normal(w)
    col_offset = sensor.black_level_dn + sensor.dsnu_dn * fixed.standard_normal(w)
    electrons = np.clip(ideal_dn * col_gain[None, :] * k, 0.0, None)
    electrons = electrons + np.sqrt(electrons + sensor.read_noise_e ** 2) * rng.standard_normal((h, w))
    dn = electrons / k + col_offset[None, :]
    return np.clip(np.floor(dn + 0.5), 0, 255).astype(np.uint8)


def scatter_particles(sizes_um, count, scene, rng, surface="front", opacity=1.0, margin=40):
    """在网格上撒颗粒（每格一个，格内随机抖动，保证互不重叠）。每种尺寸 count 个。"""
    total = len(sizes_um) * count
    cols = max(1, int(math.sqrt(total * scene.width / scene.height)))
    rows = int(math.ceil(total / cols))
    cell_w = (scene.width - 2 * margin) / cols
    cell_h = (scene.height - 2 * margin) / rows
    if cell_w < 48 or cell_h < 48:
        raise ValueError("too many particles for this image size")
    order = rng.permutation(total)
    out = []
    for n, slot in enumerate(order):
        r, c = divmod(int(slot), cols)
        out.append({
            "x": margin + (c + rng.uniform(0.3, 0.7)) * cell_w,
            "y": margin + (r + rng.uniform(0.3, 0.7)) * cell_h,
            "size_um": float(sizes_um[n % len(sizes_um)]),
            "surface": surface, "opacity": float(opacity),
        })
    return out


def write_truth(path, particles):
    with open(path, "x", newline="", encoding="utf-8") as f:
        f.write("x,y,size_um,surface,opacity\n")
        for p in particles:
            f.write(f"{p['x']:.3f},{p['y']:.3f},{p['size_um']:g},{p['surface']},{p['opacity']:g}\n")


def write_pgm(path, image):
    with open(path, "xb") as f:
        f.write(b"P5\n%d %d\n255\n" % (image.shape[1], image.shape[0]))
        f.write(image.tobytes())


def main(argv=None):
    parser = argparse.ArgumentParser(description="Simulate particles on glass as seen by the line-scan camera")
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--sizes", default="30,20,10", help="颗粒直径 µm，逗号分隔")
    parser.add_argument("--count", type=int, default=100, help="每种尺寸多少个")
    parser.add_argument("--mode", choices=["bright", "dark"], default="bright")
    parser.add_argument("--opacity", type=float, default=1.0, help="1 = 完全不透光；透明玻璃屑约 0.3–0.8")
    parser.add_argument("--surface", choices=["front", "back"], default="front")
    parser.add_argument("--width", type=int, default=4096)
    parser.add_argument("--height", type=int, default=1000)
    parser.add_argument("--background", type=float, default=None, help="背景灰度（明场默认 170，暗场默认 6）")
    parser.add_argument("--f-number", type=float, default=5.6)
    parser.add_argument("--thickness-mm", type=float, default=0.7)
    parser.add_argument("--exposure-us", type=float, default=50.0)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args(argv)

    optics = OpticsModel(f_number=args.f_number, glass_thickness_mm=args.thickness_mm,
                         exposure_us=args.exposure_us)
    sensor = SensorModel(gain=args.gain)
    scene = SceneModel(mode=args.mode, width=args.width, height=args.height)
    scene.background_dn = args.background if args.background is not None else (170.0 if args.mode == "bright" else 6.0)
    rng = np.random.default_rng(args.seed)
    sizes = [float(s) for s in args.sizes.split(",")]
    particles = scatter_particles(sizes, args.count, scene, rng, args.surface, args.opacity)

    args.out_dir.mkdir(parents=True, exist_ok=False)
    write_pgm(args.out_dir / "clean.pgm", render([], optics, sensor, scene, rng))
    write_pgm(args.out_dir / "scene.pgm", render(particles, optics, sensor, scene, rng))
    write_truth(args.out_dir / "truth.csv", particles)
    (args.out_dir / "params.json").write_text(json.dumps(
        {"optics": asdict(optics), "sensor": asdict(sensor), "scene": asdict(scene),
         "derived": {"magnification": optics.magnification, "object_pixel_um": optics.object_pixel_um,
                     "lens_sigma_px": optics.lens_sigma_px(), "motion_px": optics.motion_px(),
                     "back_surface_blur_px": optics.defocus_blur_px("back"),
                     "depth_of_field_um_1px": optics.depth_of_field_um(1.0)}},
        indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"{len(particles)} particles -> {args.out_dir} (scene.pgm, clean.pgm, truth.csv, params.json)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
