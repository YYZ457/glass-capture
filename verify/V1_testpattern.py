"""V1 · 收到的图和相机测试图案逐像素一致吗？

回答的问题：从相机到电脑的整条链路（Camera Link → 卡 → PCIe → DDR → 程序 → 传输）有没有改动任何一个像素。
通过之后，后面图像出任何问题都只可能是光学问题。

前提：相机能打开测试图案（Windows + VisionKit，或 board/camera_ctrl 写好之后）。
相机寄存器 0x01100000（[参考8K]）：1 黑，2 白，3 水平灰度渐变，4 垂直渐变。

❓ 渐变的确切公式手册里没写。这里把常见的几种都试一遍，报告哪一种完全吻合：
   水平：x % 256、x * 256 // width；垂直：(y + 起始值) % 256（起始值从第一行推出）。
   全都不吻合时，打印第一行前 16 个值，拿去和手册/VisionKit 截图比对。

输出：不一致像素数、第一个不一致的位置；退出码 0 = 某个公式完全一致，1 = 都不一致。
用法：python -I verify/V1_testpattern.py FRAME.pgm --pattern ramp|vramp|black|white
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pgmio import read_pgm  # noqa: E402


def expected_images(pattern, image):
    """返回 [(公式名, 期望图), ...]。"""
    height, width = image.shape
    x = np.arange(width, dtype=np.int64)
    y = np.arange(height, dtype=np.int64)
    if pattern == "black":
        return [("all 0", np.zeros_like(image))]
    if pattern == "white":
        return [("all 255", np.full_like(image, 255))]
    if pattern == "ramp":
        rows = [("x % 256", x % 256), ("x * 256 // width", x * 256 // width)]
        return [(name, np.broadcast_to(r.astype(np.uint8), image.shape)) for name, r in rows]
    if pattern == "vramp":
        start = int(image[0, 0])
        col = ((y + start) % 256).astype(np.uint8)
        return [(f"(y + {start}) % 256", np.broadcast_to(col[:, None], image.shape))]
    raise ValueError(pattern)


def compare(image, expected):
    diff = image != expected
    count = int(diff.sum())
    first = None
    if count:
        yy, xx = np.argwhere(diff)[0]
        first = (int(xx), int(yy), int(image[yy, xx]), int(expected[yy, xx]))
    return count, first


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("pgm")
    parser.add_argument("--pattern", required=True, choices=("ramp", "vramp", "black", "white"))
    args = parser.parse_args(argv)

    image = read_pgm(args.pgm)
    print(f"{args.pgm}: {image.shape[1]} x {image.shape[0]}")
    ok = False
    for name, expected in expected_images(args.pattern, image):
        count, first = compare(image, expected)
        if count == 0:
            print(f"  {name:20s} MATCH (all {image.size} pixels)")
            ok = True
        else:
            x, y, got, want = first
            print(f"  {name:20s} {count} mismatched pixels; first at x={x} y={y}: got {got}, expected {want}")
    if not ok:
        print("  first row[:16] =", image[0, :16].tolist())
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
