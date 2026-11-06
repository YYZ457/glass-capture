"""V3 · 暗场统计：相机的暗噪声多大？两张"纯黑图"为什么不一样？  ⬜

背景：2026-10-30 盖着镜头盖采了两帧，03:01 那张压缩后 26 KB（几乎全是一个值），
03:06 那张压缩后 1.7 MB（大量变化）。可能是预热后噪声变大，也可能是数据本身有问题。

输出（每张图）：
  - 整体均值、标准差、最小、最大、直方图
  - 逐列均值 / 标准差（固定图案噪声，也是 sg_background 标定的来源）
  - 逐行均值（看有没有整行跳变 → 提示数据错位）
这些数字也是信噪比预算的起点：1 µm / 5 µm / 20 µm 颗粒的信号要比这里的噪声大多少才能检出。

用法：python -I verify/V3_dark_stats.py A.pgm [B.pgm ...]
"""
import sys


def main() -> int:
    print("V3: not implemented yet", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
