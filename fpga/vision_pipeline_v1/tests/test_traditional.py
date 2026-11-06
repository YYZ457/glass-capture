"""Run from package root: python -m unittest discover -s tests -p test_traditional.py."""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from traditional import (BoxConfig, Candidate, ComponentConfig, CorrectionConfig,
                         box_response, connected_components, correct_dark_flat,
                         crop_roi, iter_tiles, resize_nearest, threshold_mask,
                         tiled_box_response)


class TraditionalTests(unittest.TestCase):
    def test_no_calibration_copies_and_dark_subtraction_saturates(self):
        image = np.array([[0, 50, 255]], np.uint8)
        output = correct_dark_flat(image)
        np.testing.assert_array_equal(output.image, image)
        self.assertFalse(np.shares_memory(image, output.image))
        self.assertTrue(output.valid.all())
        result = correct_dark_flat(image, np.full(image.shape, 30, np.uint8))
        np.testing.assert_array_equal(result.image, [[0, 20, 225]])

    def test_flat_rounding_clipping_and_invalid_denominators(self):
        image = np.array([[11, 12, 255, 10, 10]], np.uint8)
        dark = np.array([[10, 10, 0, 10, 20]], np.uint8)
        flat = np.array([[12, 14, 10, 10, 10]], np.uint8)
        result = correct_dark_flat(image, dark, flat,
                                   CorrectionConfig(target_dn=255, invalid_fill=7))
        np.testing.assert_array_equal(result.image, [[128, 128, 255, 7, 7]])
        np.testing.assert_array_equal(result.valid, [[True, True, True, False, False]])
        high_minimum = correct_dark_flat(image, dark, flat,
                                        CorrectionConfig(min_denominator=3))
        self.assertFalse(high_minimum.valid[0, 0])
        # Flat-only also uses dark=0, and 3*100/4=75.
        np.testing.assert_array_equal(correct_dark_flat(np.array([[3]], np.uint8),
            flat=np.array([[4]], np.uint8), config=CorrectionConfig(target_dn=100)).image, [[75]])

    def test_constant_and_radius_zero_have_no_response(self):
        for radius in (0, 1, 4):
            result = box_response(np.full((5, 7), 112, np.uint8), BoxConfig(radius))
            self.assertTrue(result.valid.all())
            self.assertFalse(result.signed.any())
            self.assertFalse(threshold_mask(result, 1).any())
            np.testing.assert_array_equal(result.background, 112)

    def test_box_matches_explicit_sum_at_borders(self):
        image = np.array([[0, 1, 2], [100, 254, 255]], np.uint8)
        for radius in (1, 2, 4):
            padded = np.pad(image, radius, mode="edge").astype(np.int64)
            expected = np.empty(image.shape, np.uint8)
            count = (2 * radius + 1) ** 2
            for y in range(image.shape[0]):
                for x in range(image.shape[1]):
                    total = padded[y:y+2*radius+1, x:x+2*radius+1].sum()
                    expected[y, x] = (total + count//2)//count
            result = box_response(image, BoxConfig(radius))
            np.testing.assert_array_equal(result.background, expected)
            self.assertEqual(result.signed.dtype, np.int16)
            self.assertEqual(result.response.dtype, np.uint8)

    def test_bright_dark_points_and_polarity(self):
        image = np.full((9, 11), 100, np.uint8)
        image[2, 2], image[6, 8] = 190, 10
        both = box_response(image, BoxConfig(1, "both"))
        self.assertEqual(both.signed[2, 2], 80)
        self.assertEqual(both.signed[6, 8], -80)
        mask = threshold_mask(both, 80)
        self.assertEqual(int(mask.sum()), 2)
        self.assertEqual(connected_components(mask, both.signed), [
            Candidate((2, 2, 3, 3), "bright", 1, 80),
            Candidate((8, 6, 9, 7), "dark", 1, 80)])
        bright = box_response(image, BoxConfig(1, "bright"))
        dark = box_response(image, BoxConfig(1, "dark"))
        self.assertFalse(threshold_mask(bright, 80)[6, 8])
        self.assertFalse(threshold_mask(dark, 80)[2, 2])

    def test_invalid_calibration_neighborhood_cannot_create_candidates(self):
        image = np.full((7, 7), 100, np.uint8)
        flat = np.full(image.shape, 200, np.uint8)
        flat[3, 3] = 0
        corrected = correct_dark_flat(image, flat=flat)
        result = box_response(corrected.image, BoxConfig(1), corrected.valid)
        self.assertFalse(result.valid[2:5, 2:5].any())
        self.assertEqual(int(result.valid.sum()), 40)
        self.assertFalse(threshold_mask(result, 1).any())
        invalid_border = np.ones((3, 3), bool)
        invalid_border[0, 0] = False
        output = box_response(np.zeros((3, 3), np.uint8), BoxConfig(1), invalid_border)
        np.testing.assert_array_equal(output.valid, [[False, False, True],
                                                   [False, False, True],
                                                   [True, True, True]])

    def test_components_diagonal_opposite_sign_edges_sort_and_limits(self):
        signed = np.array([[10, -10, 0, 0, 90],
                           [0, 20, 0, 0, 0],
                           [0, 0, 30, 0, 0],
                           [-80, 0, 0, 0, -70]], np.int16)
        mask = signed != 0
        expected = [Candidate((4, 0, 5, 1), "bright", 1, 90),
                    Candidate((0, 3, 1, 4), "dark", 1, 80),
                    Candidate((4, 3, 5, 4), "dark", 1, 70),
                    Candidate((0, 0, 3, 3), "bright", 3, 30),
                    Candidate((1, 0, 2, 1), "dark", 1, 10)]
        self.assertEqual(connected_components(mask, signed), expected)
        self.assertEqual(connected_components(mask, signed, ComponentConfig(max_candidates=2)), expected[:2])
        self.assertEqual(connected_components(mask, signed, ComponentConfig(min_area=2)), expected[3:4])
        self.assertEqual(connected_components(mask, signed, ComponentConfig(max_area=1)), expected[:3]+expected[4:])
        self.assertEqual(connected_components(mask, signed, ComponentConfig(max_candidates=0)), [])
        self.assertEqual(connected_components(np.zeros(mask.shape, bool), signed), [])

    def test_component_ties_sort_by_area_then_coordinates(self):
        signed = np.array([[50, 0, 0, 50], [0, 0, 0, 50], [0, 0, 0, 0], [50, 0, -50, 0]], np.int16)
        candidates = connected_components(signed != 0, signed)
        self.assertEqual([c.bbox for c in candidates], [(3, 0, 4, 2), (0, 0, 1, 1), (0, 3, 1, 4), (2, 3, 3, 4)])

    def test_roi_half_open_copy_and_floor_nearest(self):
        image = np.arange(20, dtype=np.uint8).reshape(4, 5)
        crop = crop_roi(image, (1, 1, 4, 3))
        np.testing.assert_array_equal(crop, [[6, 7, 8], [11, 12, 13]])
        self.assertFalse(np.shares_memory(image, crop))
        enlarged = crop_roi(image, (1, 1, 4, 3), (3, 5))
        np.testing.assert_array_equal(enlarged, [[6, 6, 7, 7, 8],
                                               [6, 6, 7, 7, 8],
                                               [11, 11, 12, 12, 13]])
        np.testing.assert_array_equal(resize_nearest(image, (2, 2)), [[0, 2], [10, 12]])

    def test_tiles_cover_once_and_map_to_input(self):
        image = np.arange(77, dtype=np.uint8).reshape(7, 11)
        coverage = np.zeros(image.shape, np.uint8)
        tiles = list(iter_tiles(image.shape, (3, 4), 2))
        self.assertEqual(len(tiles), 9)
        self.assertEqual(tiles[0].read_bbox, (0, 0, 6, 5))
        self.assertEqual(tiles[-1].core_bbox, (8, 6, 11, 7))
        for tile in tiles:
            x0, y0, x1, y1 = tile.core_bbox
            coverage[y0:y1, x0:x1] += 1
            rx0, ry0, rx1, ry1 = tile.read_bbox
            np.testing.assert_array_equal(crop_roi(image, tile.core_bbox),
                crop_roi(image[ry0:ry1, rx0:rx1], tile.core_in_read))
        np.testing.assert_array_equal(coverage, 1)

    def test_full_and_tile_responses_identical_including_bad_pixels(self):
        rng = np.random.default_rng(27)
        image = rng.integers(0, 256, (17, 23), dtype=np.uint8)
        valid = rng.random(image.shape) > .03
        for radius in (0, 1, 3):
            for polarity in ("bright", "dark", "both"):
                config = BoxConfig(radius, polarity)
                full = box_response(image, config, valid)
                for core_shape in ((1, 1), (4, 5), (30, 30)):
                    tiled = tiled_box_response(image, config, valid, core_shape, radius+1)
                    for name in ("background", "signed", "response", "valid"):
                        np.testing.assert_array_equal(getattr(tiled, name), getattr(full, name))
                    np.testing.assert_array_equal(threshold_mask(tiled), threshold_mask(full))

    def test_boundary_crossing_component_matches_after_stitch(self):
        image = np.full((9, 13), 100, np.uint8)
        image[3:6, 5:8] = 200  # crosses both axes of the (4,6) cores
        image[0, 0], image[-1, -1] = 0, 255
        full = box_response(image, BoxConfig(2))
        tiled = tiled_box_response(image, BoxConfig(2), core_shape=(4, 6))
        a = connected_components(threshold_mask(full, 30), full.signed)
        b = connected_components(threshold_mask(tiled, 30), tiled.signed)
        self.assertEqual(a, b)
        self.assertIn((5, 3, 8, 6), [c.bbox for c in a])

    def test_bad_images_masks_calibration_and_responses_rejected(self):
        image = np.zeros((3, 4), np.uint8)
        for bad in (image.astype(np.float32), image.astype(bool), np.zeros((3, 4, 1), np.uint8), [[1, 2]]):
            with self.assertRaises(TypeError):
                box_response(bad)
            with self.assertRaises(TypeError):
                correct_dark_flat(bad)
        with self.assertRaises(ValueError):
            box_response(np.zeros((0, 4), np.uint8))
        with self.assertRaises(ValueError):
            correct_dark_flat(image, flat=np.zeros((3, 3), np.uint8))
        with self.assertRaises(TypeError):
            box_response(image, valid=np.ones(image.shape, np.uint8))
        with self.assertRaises(ValueError):
            box_response(image, valid=np.ones((1, 1), bool))
        with self.assertRaises(TypeError):
            connected_components(np.ones(image.shape, np.uint8), image.astype(np.int16))
        with self.assertRaises(TypeError):
            connected_components(np.zeros(image.shape, bool), image)
        with self.assertRaises(ValueError):
            connected_components(np.ones(image.shape, bool), np.zeros(image.shape, np.int16))
        with self.assertRaises(ValueError):
            connected_components(np.zeros(image.shape, bool), np.full(image.shape, 256, np.int16))

    def test_invalid_parameters_rejected(self):
        image = np.zeros((3, 4), np.uint8)
        calls = [lambda: BoxConfig(-1), lambda: BoxConfig(1, "unknown"),
                 lambda: CorrectionConfig(target_dn=256), lambda: CorrectionConfig(invalid_fill=-1),
                 lambda: CorrectionConfig(min_denominator=0), lambda: ComponentConfig(min_area=0),
                 lambda: ComponentConfig(min_area=3, max_area=2), lambda: ComponentConfig(max_candidates=-1),
                 lambda: threshold_mask(box_response(image), 0),
                 lambda: threshold_mask(box_response(image), 256),
                 lambda: crop_roi(image, (0, 0, 5, 3)), lambda: crop_roi(image, (0, 0, 0, 1)),
                 lambda: crop_roi(image, (-1, 0, 2, 1)), lambda: resize_nearest(image, (0, 2)),
                 lambda: list(iter_tiles(image.shape, (0, 4))),
                 lambda: list(iter_tiles(image.shape, halo=-1)),
                 lambda: tiled_box_response(image, BoxConfig(2), halo=1)]
        for call in calls:
            with self.subTest(call=call), self.assertRaises(ValueError):
                call()
        for call in (lambda: BoxConfig(True), lambda: BoxConfig(1.5),
                     lambda: crop_roi(image, (0., 0, 1, 1)),
                     lambda: resize_nearest(image, (True, 2)),
                     lambda: list(iter_tiles(image.shape, halo=1.5))):
            with self.assertRaises(TypeError):
                call()


if __name__ == "__main__":
    unittest.main()
