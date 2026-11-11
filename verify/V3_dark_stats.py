"""V3 · 暗场统计：相机的暗噪声多大？两张"纯黑图"为什么不一样？

背景：2026-10-30 盖着镜头盖采了两帧，03:01 那张压缩后 26 KB（几乎全是一个值），
03:06 那张压缩后 1.7 MB（大量变化）。可能是预热后噪声变大，也可能是数据本身有问题。

输出（每张图）：
  - 整体均值、标准差、最小、最大、不同取值个数、gzip 压缩后大小、直方图（只列出现过的值）
  - 逐列均值 / 标准差的范围（固定图案噪声）；--save-background 存成 CSV，就是 sg_background 的标定
  - 逐行均值：和中位数差得多的行（提示整行跳变 / 数据错位）
这些数字也是信噪比预算的起点：颗粒的信号要比这里的噪声大多少才能检出。

用法：python -I verify/V3_dark_stats.py A.pgm [B.pgm ...] [--save-background bg.csv]
     （多张图时 --save-background 用全部图的平均）
"""
import argparse
import gzip
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgmio import read_pgm  # noqa: E402

ROW_OUTLIER_SIGMA = 5.0


def describe(path, image):
    pixels = image.astype(np.float64)
    values, counts = np.unique(image, return_counts=True)
    col_mean = pixels.mean(axis=0)
    col_std = pixels.std(axis=0)
    row_mean = pixels.mean(axis=1)

    print(f"== {path}: {image.shape[1]} x {image.shape[0]}")
    print(f"mean {pixels.mean():.3f}  std {pixels.std():.3f}  min {image.min()}  max {image.max()}  "
          f"distinct values {len(values)}  gzip {len(gzip.compress(image.tobytes(), 6)) / 1024:.1f} KiB")
    hist = ", ".join(f"{v}:{c}" for v, c in zip(values.tolist(), counts.tolist()))
    print(f"histogram {hist if len(values) <= 32 else hist[:300] + ' ...'}")
    print(f"column mean {col_mean.min():.2f}..{col_mean.max():.2f} (spread std {col_mean.std():.3f})   "
          f"column std {col_std.min():.2f}..{col_std.max():.2f}")

    med = float(np.median(row_mean))
    mad = float(np.median(np.abs(row_mean - med))) * 1.4826 or float(row_mean.std()) or 1.0
    bad = np.flatnonzero(np.abs(row_mean - med) > ROW_OUTLIER_SIGMA * mad)
    print(f"row mean {row_mean.min():.2f}..{row_mean.max():.2f}, median {med:.2f}; "
          f"{len(bad)} outlier rows (> {ROW_OUTLIER_SIGMA:g} sigma)" +
          (f", first {bad[:10].tolist()}" if len(bad) else ""))
    return pixels.sum(axis=0), (pixels ** 2).sum(axis=0), image.shape[0]


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("pgm", nargs="+")
    parser.add_argument("--save-background", type=Path, help="写出 x,mean,sigma 的 CSV")
    args = parser.parse_args(argv)

    total = total_sq = None
    rows = 0
    width = None
    for path in args.pgm:
        image = read_pgm(path)
        s, sq, n = describe(path, image)
        if width is None:
            width, total, total_sq = image.shape[1], s, sq
        elif image.shape[1] != width:
            print(f"  (width differs, not included in background)")
            continue
        else:
            total, total_sq = total + s, total_sq + sq
        rows += n

    if args.save_background:
        mean = total / rows
        sigma = np.sqrt(np.maximum(total_sq / rows - mean ** 2, 0))
        with open(args.save_background, "x") as f:
            f.write("x,mean,sigma\n")
            for x in range(width):
                f.write(f"{x},{mean[x]:.4f},{sigma[x]:.4f}\n")
        print(f"background ({rows} rows) -> {args.save_background}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
