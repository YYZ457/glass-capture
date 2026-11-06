"""V2 · 行完整性：有没有丢行、错位、宽度被切分？  ⬜

回答的问题：
  1. 宽度到底是 4096 还是 16384？（遮住传感器左半边采一帧：
     每行都有明暗分界 → 相机输出 4096；每 4 行一组明暗交替 → 相机发 16384、卡按 4096 切行）
  2. 相邻行之间有没有突然的错位（列方向平移）？
  3. 连续多块之间块号是否连续（读 SGJSON 日志的 block_id）？

输入：PGM 文件，或一段 SGJSON 日志。
不需要镜头，不需要测试图案，**现在就能做**。

用法：python -I verify/V2_line_integrity.py FRAME.pgm
"""
import sys


def read_pgm(path: str):
    """读 P5 PGM，返回 (width, height, bytes)。TODO"""
    raise NotImplementedError


def width_split_test(width: int, height: int, data: bytes) -> str:
    """判断 '每行有分界' 还是 '每 4 行一组交替'。TODO"""
    raise NotImplementedError


def main() -> int:
    print("V2: not implemented yet", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
