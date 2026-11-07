"""V1 · 收到的图和相机测试图案逐像素一致吗？  ⬜

回答的问题：从相机到电脑的整条链路（Camera Link → 卡 → PCIe → DDR → 程序 → 传输）有没有改动任何一个像素。
通过之后，后面图像出任何问题都只可能是光学问题。

前提：相机能打开测试图案（Windows + VisionKit，或 board/camera_ctrl 写好之后）。
相机寄存器 0x01100000（[参考8K]）：3 = 水平灰度渐变（GreyHorizontalRamp），4 = 垂直，1 黑，2 白。
输入：一个 PGM 文件 + 图案类型（灰度渐变 / 固定条纹，按相机手册）。
输出：不一致像素数、第一个不一致的位置；退出码 0 = 完全一致。

用法：python -I verify/V1_testpattern.py FRAME.pgm --pattern ramp
"""
import sys


def expected_row(pattern: str, width: int) -> bytes:
    """按相机手册生成一行期望值。TODO：拿到手册后实现。"""
    raise NotImplementedError


def main() -> int:
    print("V1: not implemented yet", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
