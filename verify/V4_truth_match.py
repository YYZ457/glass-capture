"""V4 · 真值比对：已知颗粒能检出几个、误报几个、位置准不准？  ⬜

这是唯一直接回答"检测系统对不对"的验证，也是验收要拿出来的数字。

输入：
  - 候选列表（frame-candidates-v1 格式，来自 host/console 或 board 的 sg_candidates）
  - 真值文件：每个已知颗粒的位置（像素或 mm）和尺寸（标准微球 / 显微镜标定）
输出：召回率、误报数、位置误差分布，按颗粒尺寸分档（≥30 µm / 10–30 / <10）。

匹配规则（待定）：候选质心与真值距离 < R 像素即算命中，一一匹配。

用法：python -I verify/V4_truth_match.py CANDIDATES.json TRUTH.csv --radius 5
"""
import sys


def main() -> int:
    print("V4: not implemented yet", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
