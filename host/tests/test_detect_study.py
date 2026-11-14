"""检测方案（docs/design/detection_plan.md）用到的仿真器和算法原型的测试。纯 numpy，不需要板子。"""
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "host" / "detect"))
sys.path.insert(0, str(REPO / "verify"))

import detect_v1  # noqa: E402
import sim_particles as sim  # noqa: E402
import study_detectability as study  # noqa: E402


def test_optics_numbers_match_the_plan():
    """方案第 2 节的导出量。改了默认假设，文档里的数也要跟着改。"""
    o = sim.OpticsModel()
    assert o.magnification == pytest.approx(0.7168, abs=1e-4)
    assert o.object_pixel_um == pytest.approx(4.8828, abs=1e-4)
    assert o.depth_of_field_um(1.0) / 2 == pytest.approx(65.5, abs=0.5)
    assert o.defocus_blur_px("front") == 0.0
    assert o.defocus_blur_px("back") == pytest.approx(7.03, abs=0.02)      # 0.7 mm 玻璃
    assert o.motion_px() == pytest.approx(0.768, abs=1e-3)
    assert sim.SensorModel().noise_dn(170) == pytest.approx(2.07, abs=0.02)


@pytest.mark.parametrize("surface", ["front", "back"])
def test_blur_keeps_total_blocked_light(surface):
    """模糊只把"挡光量"摊开，总量不变（体积法量尺寸的依据）；峰值随尺寸和离焦下降。"""
    o = sim.OpticsModel()
    for d in (10.0, 30.0):
        patch, half = sim.particle_patch(d, 0.2, -0.3, surface, o)
        ideal = math.pi * (d / 2 / o.object_pixel_um) ** 2
        assert patch.shape == (2 * half + 1, 2 * half + 1)
        assert patch.sum() == pytest.approx(ideal, rel=0.03)
    front30 = sim.particle_patch(30.0, 0, 0, "front", o)[0].max()
    back30 = sim.particle_patch(30.0, 0, 0, "back", o)[0].max()
    assert front30 > 0.97 and 0.5 < back30 < 0.8


def test_render_is_reproducible_and_fixed_pattern_is_shared():
    scene = sim.SceneModel(width=512, height=64)
    a = sim.render([], sim.OpticsModel(), sim.SensorModel(), scene, np.random.default_rng(5))
    b = sim.render([], sim.OpticsModel(), sim.SensorModel(), scene, np.random.default_rng(5))
    c = sim.render([], sim.OpticsModel(), sim.SensorModel(), scene, np.random.default_rng(6))
    assert a.dtype == np.uint8 and a.shape == (64, 512) and np.array_equal(a, b)
    # 不同的随机数 = 不同的时间噪声，但列固定图案相同：两张图的列均值差只有时间噪声那么大
    diff = a.astype(float).mean(axis=0) - c.astype(float).mean(axis=0)
    assert abs(diff.mean()) < 0.2 and diff.std() < 0.6


def test_label_mask_is_8_connected_and_merge_near_joins_fragments():
    mask = np.zeros((8, 12), dtype=bool)
    mask[1, 1] = mask[2, 2] = True          # 斜着相邻 → 一个
    mask[1, 9:11] = True                    # 另一个，离第一个 6 列
    mask[5, 9] = True                       # 在它下面，中间空 3 行
    comps = detect_v1.label_mask(mask)
    assert [sum(x1 - x0 for _, x0, x1 in c) for c in comps] == [2, 2, 1]
    assert len(detect_v1.merge_near(comps, 0)) == 3
    assert len(detect_v1.merge_near(comps, 3)) == 2      # 第二、三个并了，第一个离得远


def test_column_thresholds_never_fire_out_of_range():
    lo, hi = detect_v1.column_thresholds(np.array([2.0, 100.0, 252.0]), np.array([1.0, 2.0, 1.0]))
    assert lo.tolist() == [-1, 88, 236] and hi.tolist() == [8, 112, 256]


def test_detects_30um_and_measures_size_in_simulation():
    optics, sensor = sim.OpticsModel(), sim.SensorModel()
    scene = sim.SceneModel(width=1024, height=512)
    by_size, false_alarms, pixels = study.run_case(optics, sensor, scene, [30, 15], 20)
    assert pixels == 1024 * 512 and false_alarms == 0
    assert by_size[30]["hit"] == 20 and by_size[15]["hit"] == 20
    assert np.mean(by_size[30]["d_fwhm"]) == pytest.approx(30, abs=2.5)
    assert np.mean(by_size[30]["d_vol"]) == pytest.approx(30, abs=1.5)
    assert by_size[30]["particle"] == 20


def test_back_surface_particles_are_flagged_blurred():
    optics, sensor = sim.OpticsModel(), sim.SensorModel()
    scene = sim.SceneModel(width=1024, height=512)
    by_size, _, _ = study.run_case(optics, sensor, scene, [30], 20, surface="back")
    assert by_size[30]["hit"] == 20 and by_size[30]["particle"] == 0
    assert np.mean(by_size[30]["edge"]) > 2.0


def test_cli_chain_sim_detect_truth_match(tmp_path):
    """附录 A 的三条命令：仿真 → 检测 → V4 真值比对。"""
    out = tmp_path / "sim"
    run = lambda *a: subprocess.run([sys.executable, "-I", *map(str, a)], capture_output=True, text=True, check=True)
    run(REPO / "host/detect/sim_particles.py", out, "--sizes", "30,20", "--count", "10",
        "--width", "1024", "--height", "512")
    assert (out / "params.json").exists() and (out / "truth.csv").read_text().startswith("x,y,size_um")
    cands = run(REPO / "host/detect/detect_v1.py", out / "scene.pgm", "--clean", out / "clean.pgm").stdout
    doc = json.loads(cands)
    assert doc["count"] == 20 and all(c["class"] == "particle" for c in doc["candidates"])
    (out / "cands.json").write_text(cands, encoding="utf-8")
    report = run(REPO / "verify/V4_truth_match.py", out / "cands.json", out / "truth.csv", "--radius", "6").stdout
    assert "recall 1.000" in report and "false alarms 0" in report
