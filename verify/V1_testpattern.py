"""V1 · 收到的图和相机测试图案逐像素一致吗？

回答的问题：从相机到电脑的整条链路（Camera Link → 卡 → PCIe → DDR → 程序 → 传输）有没有改动任何一个像素。
通过之后，后面图像出任何问题都只可能是光学问题。

前提：板上 `sg3000-capture camera-set TestPattern 3`（相机断电恢复）。
相机寄存器 0x01100000（16K XML）：0 关，1 黑，2 白，3 GreyHorizontalRamp，5 GreyHorizontalRampMoving。没有垂直渐变。

❓ 渐变的确切公式手册里没写。这里把常见的几种都试一遍，报告哪一种完全吻合：
   水平：x % 256、(x + 起点) % 256、x * 256 // width、每行各自起点（移动渐变 5 用）；
   垂直 vramp 留给其他相机。
   全都不吻合时，打印第一行前 16 个值，拿去和手册截图比对。
不管吻合与否都打印"位统计"：8 个位各自有没有出现过 1、按列 %3 分开的 OR 掩码。
   渐变图里 8 个位都应该出现；某一位从来不出现 = 这一位在链路上丢了（2026-11-14 实拍 OR 只到 0x3f 的疑问就靠这个回答）。

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
        start = int(image[0, 0])
        rows = [("x % 256", x % 256), (f"(x + {start}) % 256", (x + start) % 256),
                ("x * 256 // width", x * 256 // width)]
        out = [(name, np.broadcast_to(r.astype(np.uint8), image.shape)) for name, r in rows]
        per_row = ((x[None, :] + image[:, :1].astype(np.int64)) % 256).astype(np.uint8)
        out.append(("(x + row start) % 256  [moving]", per_row))
        return out
    if pattern == "vramp":
        start = int(image[0, 0])
        col = ((y + start) % 256).astype(np.uint8)
        return [(f"(y + {start}) % 256", np.broadcast_to(col[:, None], image.shape))]
    raise ValueError(pattern)


def bit_report(image):
    """每一位有没有出现过；按列 %3（Base 3 Tap 的三路）分开的 OR 掩码。"""
    total = int(np.bitwise_or.reduce(image, axis=None))
    seen = [b for b in range(8) if total >> b & 1]
    print(f"  bits: OR=0x{total:02x}  seen={seen}  missing={[b for b in range(8) if b not in seen]}"
          f"  min={int(image.min())} max={int(image.max())}")
    taps = [int(np.bitwise_or.reduce(image[:, k::3], axis=None)) for k in range(3)]
    print("  OR by column %3 (Base 3-tap):", " ".join(f"0x{v:02x}" for v in taps))


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
    bit_report(image)
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
