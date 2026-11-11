"""host/detect/candidates.py 的手算用例（和 board/processing/sg_candidates.c 同一套期望）。"""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "detect"))
from candidates import find_candidates  # noqa: E402


class CandidatesTest(unittest.TestCase):
    def setUp(self):
        img = np.full((32, 64), 50, np.uint8)
        img[5:8, 10:13] = 100
        img[6, 11] = 200                      # 3x3 亮团，中心最亮
        img[20, 40] = img[21, 41] = 0         # 对角相连的暗点，8 邻接 → 一个
        img[30, 60] = 255                     # 孤立单像素，面积 1 被过滤
        img[10:15, 30] = img[10:15, 34] = 150
        img[14, 30:35] = 150                  # U 形，两臂在底部相连
        self.img = img
        self.mean = np.full(64, 50, np.float32)

    def test_known_blobs(self):
        out, truncated = find_candidates(self.img, 20, min_area=2, mean=self.mean)
        self.assertFalse(truncated)
        self.assertEqual([c["bbox"] for c in out], [[10, 5, 13, 8], [30, 10, 35, 15], [40, 20, 42, 22]])
        self.assertEqual([c["area"] for c in out], [9, 13, 2])
        self.assertEqual(out[0]["peak"], 150)
        self.assertAlmostEqual(out[0]["cx"], 11.0, places=4)
        self.assertAlmostEqual(out[0]["cy"], 6.0, places=4)

    def test_truncation_is_reported(self):
        out, truncated = find_candidates(self.img, 20, min_area=2, max_candidates=2, mean=self.mean)
        self.assertTrue(truncated)
        self.assertEqual(len(out), 2)

    def test_area_limit(self):
        out, _ = find_candidates(self.img, 20, min_area=2, max_area=9, mean=self.mean)
        self.assertEqual([c["area"] for c in out], [9, 2])


if __name__ == "__main__":
    unittest.main()
