"""Host-only streaming, halo and tile tests; no FPGA/device is required."""

import unittest
import sys
from pathlib import Path
from dataclasses import FrozenInstanceError
from itertools import cycle

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from strip_cache import (
    StripCache, Stripe, iter_tiles,
)


def make_frame(width, height):
    return bytes((x * 19 + y * 37 + (x ^ y)) % 256
                 for y in range(height) for x in range(width))


def stream_frame(cache, frame, chunks):
    """Model a producer that retains and retries each unconsumed suffix."""
    stripes = []
    offset = 0
    for size in cycle(chunks):
        if offset >= len(frame):
            break
        end = min(len(frame), offset + size)
        fragment = memoryview(frame)[offset:end]
        fragment_offset = 0
        while fragment_offset < len(fragment):
            consumed = cache.push(fragment[fragment_offset:])
            fragment_offset += consumed
            offset += consumed
            if cache.ready:
                stripes.append(cache.pop())
            elif consumed == 0:
                raise AssertionError("producer cannot make progress")
            if cache.buffered_bytes > cache.max_buffered_bytes:
                raise AssertionError("cache payload exceeded bound")
    cache.finish()
    while cache.ready:
        stripes.append(cache.pop())
    return stripes


class StripCacheTests(unittest.TestCase):
    def check_reconstruction(self, width, height, core_rows, radius, chunks):
        frame = make_frame(width, height)
        cache = StripCache(width, height, core_rows, radius, frame_id="camera-42")
        stripes = stream_frame(cache, frame, chunks)
        reconstructed = bytearray(len(frame))
        seen = bytearray(len(frame))
        expected_core_start = 0
        for stripe in stripes:
            self.assertEqual(stripe.core_start, expected_core_start)
            self.assertEqual(stripe.source_y, max(0, expected_core_start - radius))
            expected_end = min(height, expected_core_start + stripe.core_rows + radius)
            self.assertEqual(stripe.data, frame[stripe.source_y * width:expected_end * width])
            self.assertEqual(stripe.core_y, expected_core_start - stripe.source_y)
            for tile in iter_tiles(stripe, core_width=113):
                self.assertEqual(tile.frame_id, "camera-42")
                self.assertEqual(tile.global_y, expected_core_start)
                self.assertEqual(tile.core_height, stripe.core_rows)
                for dy in range(tile.core_height):
                    src = (tile.core_y + dy) * width + tile.core_x
                    dest = (tile.global_y + dy) * width + tile.core_x
                    reconstructed[dest:dest + tile.core_width] = stripe.data[src:src + tile.core_width]
                    for i in range(dest, dest + tile.core_width):
                        seen[i] += 1
            expected_core_start += stripe.core_rows
        self.assertEqual(expected_core_start, height)
        self.assertEqual(reconstructed, frame)
        self.assertEqual(set(seen), {1})
        self.assertTrue(cache.done)
        self.assertEqual(cache.buffered_bytes, 0)
        self.assertLessEqual(cache.high_water_bytes, cache.max_buffered_bytes)
        return cache, stripes

    def test_16k_135_rows_byte_fragment_boundaries(self):
        cache, stripes = self.check_reconstruction(16384, 135, 64, 7,
                                                   [1, 3, 65536, 16383, 131073])
        self.assertEqual([s.core_rows for s in stripes], [64, 64, 7])
        self.assertEqual([s.source_rows for s in stripes], [71, 78, 14])
        self.assertEqual(cache.high_water_bytes, 16384 * 78)

    def test_degenerate_dimensions_and_radius(self):
        for width, height, core, radius in [
            (1, 1, 64, 7), (16384, 1, 64, 0), (1, 137, 64, 0),
            (17, 3, 64, 7), (5, 41, 1, 7), (257, 19, 3, 7),
            (255, 129, 64, 1), (259, 10, 2, 0),
        ]:
            with self.subTest(width=width, height=height, core=core, radius=radius):
                self.check_reconstruction(width, height, core, radius, [1, 3, 65536])

    def test_ready_backpressure_snapshot_and_mutable_input(self):
        frame = bytearray(make_frame(13, 19))
        original = bytes(frame)
        cache = StripCache(13, 19, 4, 2)
        consumed = cache.push(frame)
        self.assertEqual(consumed, 13 * 6)
        self.assertTrue(cache.ready)
        self.assertEqual(cache.push(memoryview(frame)[consumed:]), 0)
        first = cache.pop()
        snapshot = first.data
        frame[:consumed] = b"\xff" * consumed
        self.assertEqual(first.data, original[:consumed])
        with self.assertRaises(FrozenInstanceError):
            first.core_y = 1
        offset = consumed
        while offset < len(frame):
            offset += cache.push(memoryview(frame)[offset:])
            if cache.ready:
                stripe = cache.pop()
                start = stripe.source_y * cache.width
                self.assertEqual(stripe.data, original[start:start + len(stripe.data)])
        cache.finish()
        while cache.ready:
            cache.pop()
        self.assertEqual(first.data, snapshot)

    def test_last_overlapping_stripe_is_ready_without_another_push(self):
        width, height = 3, 135
        frame = make_frame(width, height)
        cache = StripCache(width, height, 64, 7)
        used = cache.push(frame)
        self.assertEqual(cache.pop().core_start, 0)
        used += cache.push(memoryview(frame)[used:])
        self.assertEqual(used, len(frame))
        self.assertEqual(cache.pop().core_start, 64)
        self.assertTrue(cache.ready)
        cache.finish()
        self.assertEqual(cache.pop().core_start, 128)
        self.assertTrue(cache.done)

    def test_partial_row_and_premature_finish_are_recoverable(self):
        cache = StripCache(5, 2, 1, 0)
        self.assertEqual(cache.push(b"abc"), 3)
        self.assertEqual(cache.buffered_bytes, 3)
        with self.assertRaisesRegex(ValueError, "partial row"):
            cache.finish()
        self.assertEqual(cache.push(b"de"), 2)
        with self.assertRaisesRegex(ValueError, "truncated frame"):
            cache.finish()
        self.assertEqual(cache.pop().data, b"abcde")
        self.assertEqual(cache.push(b"fghij"), 5)
        cache.finish()  # Closing input does not discard the final pending stripe.
        self.assertEqual(cache.pop().data, b"fghij")
        self.assertTrue(cache.done)
        cache.finish()  # Idempotent completion.
        self.assertEqual(cache.push(b""), 0)
        with self.assertRaises(ValueError):
            cache.push(b"x")
        with self.assertRaises(LookupError):
            cache.pop()

    def test_excess_rejected_atomically_including_when_ready(self):
        cache = StripCache(2, 2, 1, 0)
        with self.assertRaises(ValueError):
            cache.push(b"12345")
        self.assertEqual(cache.bytes_received, 0)
        self.assertEqual(cache.push(b"1234"), 2)
        with self.assertRaises(ValueError):
            cache.push(b"345")
        self.assertEqual(cache.bytes_received, 2)
        cache.pop()
        self.assertEqual(cache.push(b"34"), 2)
        with self.assertRaises(ValueError):
            cache.push(b"5")

    def test_byteslike_and_bad_fragments(self):
        cache = StripCache(4, 2, 1, 0)
        with self.assertRaises(TypeError):
            cache.push("1234")
        with self.assertRaises(ValueError):
            cache.push(memoryview(b"12345678")[::2])
        self.assertEqual(cache.push(memoryview(bytearray(b"abcd"))), 4)
        self.assertEqual(cache.pop().data, b"abcd")

    def test_constructor_validation(self):
        for kwargs in [dict(width=0), dict(width=16385), dict(total_rows=0),
                       dict(total_rows=65537), dict(core_rows=0), dict(core_rows=65),
                       dict(radius=-1), dict(radius=8)]:
            args = dict(width=1, total_rows=1)
            args.update(kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                StripCache(**args)
        for kwargs in [dict(width=True), dict(total_rows=2.0), dict(radius="1")]:
            args = dict(width=1, total_rows=1)
            args.update(kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(TypeError):
                StripCache(**args)

    def test_max_height_and_64bit_global_coordinates(self):
        self.check_reconstruction(1, 65536, 64, 7, [65536])
        big_y = (1 << 40) + 65535
        stripe = Stripe(b"abcd", 2, big_y, 1, 1, 2**40, big_y + 1)
        tiles = list(iter_tiles(stripe, 1))
        self.assertEqual([t.global_y for t in tiles], [big_y + 1, big_y + 1])
        self.assertEqual([t.core_x for t in tiles], [0, 1])
        self.assertEqual(tiles[0].frame_id, 2**40)
        for width in [0, 257]:
            with self.assertRaises(ValueError):
                list(iter_tiles(stripe, width))

    def test_halo_pixels_match_global_replicate_for_every_core(self):
        width, height, radius = 17, 135, 7
        frame = make_frame(width, height)
        cache = StripCache(width, height, 64, radius)
        for stripe in stream_frame(cache, frame, [1, 3, 65536]):
            for tile in iter_tiles(stripe, 5):
                for local_y in range(tile.core_y, tile.core_y + tile.core_height):
                    global_y = stripe.source_y + local_y
                    for x in range(tile.core_x, tile.core_x + tile.core_width):
                        for delta in range(-radius, radius + 1):
                            # Replication at source limits must agree with the
                            # full frame, including both internal stripe joins.
                            source_row = max(0, min(stripe.source_rows - 1, local_y + delta))
                            global_row = max(0, min(height - 1, global_y + delta))
                            self.assertEqual(stripe.data[source_row * width + x],
                                             frame[global_row * width + x])
                            source_x = max(0, min(width - 1, x + delta))
                            self.assertEqual(stripe.data[local_y * width + source_x],
                                             frame[global_y * width + source_x])


if __name__ == "__main__":
    unittest.main()
