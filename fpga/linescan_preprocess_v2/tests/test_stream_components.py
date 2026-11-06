"""Independent full-frame flood-fill oracle for the streaming CPU reference."""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from stream_components import Component, StreamComponents


def flood_fill(signed, mask, frame_id=0, min_area=1, max_area=None):
    seen = set()
    found = []
    height, width = mask.shape
    for y in range(height):
        for x in range(width):
            if not mask[y, x] or (y, x) in seen:
                continue
            positive = signed[y, x] > 0
            todo = [(y, x)]
            seen.add((y, x))
            pixels = []
            while todo:
                cy, cx = todo.pop()
                pixels.append((cy, cx))
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = cy + dy, cx + dx
                        if (0 <= ny < height and 0 <= nx < width and mask[ny, nx]
                                and (ny, nx) not in seen and (signed[ny, nx] > 0) == positive):
                            seen.add((ny, nx))
                            todo.append((ny, nx))
            if len(pixels) < min_area or (max_area is not None and len(pixels) > max_area):
                continue
            ys, xs = zip(*pixels)
            found.append(Component((min(xs), min(ys), max(xs) + 1, max(ys) + 1),
                                   "bright" if positive else "dark", len(pixels),
                                   max(abs(int(signed[py, px])) for py, px in pixels), frame_id))
    return canonical(found)


def canonical(components):
    return sorted(components, key=lambda c: (c.bbox, c.polarity, c.area, c.peak, c.frame_id))


def streamed(signed, mask, **kwargs):
    stream = StreamComponents(signed.shape[1], **kwargs)
    result = []
    for response_row, mask_row in zip(signed, mask):
        result.extend(stream.consume_row(response_row, mask_row))
        assert stream.active_state_count <= 2 * signed.shape[1]
    result.extend(stream.finish())
    assert stream.finish() == []
    assert stream.active_state_count == 0
    return canonical(result)


@pytest.mark.parametrize("seed", range(30))
def test_random_against_independent_flood_fill(seed):
    rng = np.random.default_rng(seed)
    for _ in range(12):
        shape = (int(rng.integers(1, 22)), int(rng.integers(1, 34)))
        signed = rng.integers(-255, 256, size=shape, dtype=np.int16)
        mask = (rng.random(shape) < rng.uniform(0.1, 0.95)) & (signed != 0)
        kwargs = {"frame_id": seed, "min_area": int(rng.integers(1, 6)),
                  "max_area": None if seed % 2 else 40}
        assert streamed(signed, mask, **kwargs) == flood_fill(signed, mask, **kwargs)


@pytest.mark.parametrize("values", [
    [[1, 0, 0], [0, 2, 0], [0, 0, 3]],  # diagonal connection
    [[1, -1, 1], [-1, 1, -1]],  # diagonal same-polarity, adjacent opposite
    [[1, 0, 1, 0, 1], [1, 1, 1, 1, 1], [1, 0, 1, 0, 1], [1, 1, 1, 1, 1]],
    [[1, 0, 0, -2, 0], [0, 0, 0, 0, 0], [3, 0, 4, 0, -5]],
    [[1, 1, 1, 1, 1], [1, 0, 0, 0, 1], [1, 0, 1, 0, 1], [1, 1, 1, 1, 1]],
])
def test_directed_split_merge_diagonal_islands(values):
    signed = np.array(values, dtype=np.int16)
    mask = signed != 0
    result = streamed(signed, mask)
    assert result == flood_fill(signed, mask)
    assert sum(component.area for component in result) == int(mask.sum())


def test_crosses_horizontal_core_and_vertical_stripe_boundaries():
    signed = np.zeros((130, 520), dtype=np.int16)
    signed[58:70, 250:264] = 17
    signed[60:129, 500] = -255
    signed[64, 0] = 99
    mask = signed != 0
    # Tile-core stitching belongs to the caller; the stream sees global rows.
    reconstructed = np.concatenate([signed[:, :256], signed[:, 256:512], signed[:, 512:]], axis=1)
    stream = StreamComponents(520, frame_id=7)
    result = []
    for start, stop in ((0, 64), (64, 128), (128, 130)):
        for y in range(start, stop):
            result.extend(stream.consume_row(reconstructed[y], mask[y]))
    result.extend(stream.finish())
    assert canonical(result) == flood_fill(signed, mask, frame_id=7)
    assert Component((250, 58, 264, 70), "bright", 168, 17, 7) in result


def test_no_candidate_count_truncation_and_width_16384():
    width = 16384
    row = np.zeros(width, np.int16)
    row[::2] = 1
    stream = StreamComponents(width)
    assert stream.consume_row(row, row != 0) == []
    assert stream.active_state_count == width
    result = stream.consume_row(np.zeros(width, np.int16), np.zeros(width, bool))
    assert len(result) == width // 2
    assert sum(c.area for c in result) == width // 2
    assert stream.finish() == []


def test_many_rows_single_component_retains_only_frontier():
    width, height = 31, 10000
    signed = np.full(width, -255, np.int16)
    mask = np.ones(width, bool)
    stream = StreamComponents(width, frame_id=123)
    for _ in range(height):
        assert stream.consume_row(signed, mask) == []
        assert stream.active_state_count == 2
    assert stream.finish() == [Component((0, 0, width, height), "dark", width * height, 255, 123)]
    assert stream.rows_consumed == height


def test_width_16384_full_foreground():
    width, height = 16384, 16
    signed = np.full(width, 11, np.int16)
    mask = np.ones(width, bool)
    stream = StreamComponents(width)
    for _ in range(height):
        assert stream.consume_row(signed, mask) == []
        assert stream.active_state_count == 2
    assert stream.finish() == [Component((0, 0, width, height), "bright", width * height, 11)]


def test_area_filters_wait_until_close_and_merge():
    signed = np.array([[1, 0, 1], [1, 1, 1], [0, 0, 0]], dtype=np.int16)
    mask = signed != 0
    assert streamed(signed, mask, min_area=5, max_area=5) == flood_fill(signed, mask, min_area=5, max_area=5)
    assert streamed(signed, mask, min_area=1, max_area=4) == []


def test_finish_empty_idempotent_and_refuse_after_finish():
    stream = StreamComponents(3)
    assert stream.finish() == []
    assert stream.finish() == []
    with pytest.raises(RuntimeError):
        stream.consume_row(np.ones(3, np.int16), np.ones(3, bool))


@pytest.mark.parametrize("signed,mask,error", [
    ([1, 1, 1], np.ones(3, bool), TypeError),
    (np.ones(3, np.int32), np.ones(3, bool), TypeError),
    (np.ones((1, 3), np.int16), np.ones(3, bool), TypeError),
    (np.ones(2, np.int16), np.ones(3, bool), ValueError),
    (np.ones(3, np.int16), np.ones(3, np.uint8), TypeError),
    (np.ones(3, np.int16), np.ones(2, bool), ValueError),
    (np.array([1, 0, 1], np.int16), np.ones(3, bool), ValueError),
    (np.array([1, 256, 1], np.int16), np.array([1, 0, 1], bool), ValueError),
    (np.array([1, -256, 1], np.int16), np.ones(3, bool), ValueError),
])
def test_invalid_rows_do_not_advance_or_change_component(signed, mask, error):
    stream = StreamComponents(3)
    good = np.ones(3, np.int16)
    good_mask = np.ones(3, bool)
    stream.consume_row(good, good_mask)
    with pytest.raises(error):
        stream.consume_row(signed, mask)
    assert stream.rows_consumed == 1
    assert stream.active_state_count == 2
    assert stream.consume_row(good, good_mask) == []
    assert stream.finish() == [Component((0, 0, 3, 2), "bright", 6, 1)]


@pytest.mark.parametrize("kwargs,error", [
    ({"width": 0}, ValueError), ({"width": True}, TypeError),
    ({"width": 1.5}, TypeError), ({"width": 3, "frame_id": -1}, ValueError),
    ({"width": 3, "min_area": 0}, ValueError),
    ({"width": 3, "min_area": 5, "max_area": 4}, ValueError),
    ({"width": 3, "max_area": True}, TypeError),
])
def test_invalid_configuration(kwargs, error):
    with pytest.raises(error):
        StreamComponents(**kwargs)
