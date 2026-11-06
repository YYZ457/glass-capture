"""CPU reference for row-streamed, polarity-separated 8-connectivity.

Feed full-width rows in global order, including across stripe boundaries. Tile
cores must be stitched horizontally before feeding a row. Only frontier runs
and component aggregates survive a call: no frame-sized labels or pixel lists
are retained. Runtime and emitted candidate count depend on the input; this
module makes no hardware-throughput claim.
"""
from dataclasses import dataclass
from typing import Optional, Tuple

import numpy as np


@dataclass(frozen=True)
class Component:
    bbox: Tuple[int, int, int, int]  # x0, y0, x1, y1; half-open
    polarity: str
    area: int
    peak: int  # positive absolute DN amplitude
    frame_id: int = 0


def _integer(value, name, minimum=0):
    if isinstance(value, (bool, np.bool_)) or not isinstance(value, (int, np.integer)):
        raise TypeError(f"{name} must be an integer")
    if value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return int(value)


@dataclass
class _Stats:
    x0: int
    y0: int
    x1: int
    y1: int
    bright: bool
    area: int
    peak: int


class StreamComponents:
    """Close and emit components as soon as they leave the row frontier.

    ``consume_row(signed, mask)`` accepts 1-D int16/bool NumPy arrays, each of
    exactly ``width`` elements. All signed values must be in [-255, 255], and
    true mask pixels must be nonzero. Invalid rows do not advance the stream.
    Area filters apply only when a component closes. Output has no count cap
    or global ordering; the consumer owns output storage, ranking and limits.

    ``finish()`` closes the final frontier once; subsequent calls return [].
    Feeding any row after finish raises RuntimeError. Each instance represents
    one frame and assigns its supplied frame_id to every emitted component.
    """

    def __init__(self, width, frame_id=0, min_area=1, max_area: Optional[int] = None):
        self.width = _integer(width, "width", 1)
        self.frame_id = _integer(frame_id, "frame_id")
        self.min_area = _integer(min_area, "min_area", 1)
        self.max_area = None if max_area is None else _integer(max_area, "max_area", 1)
        if self.max_area is not None and self.max_area < self.min_area:
            raise ValueError("max_area must be >= min_area")
        self._y = 0
        self._finished = False
        self._runs = []  # (x0, x1, dense component label), sorted by x0
        self._stats = []

    @property
    def rows_consumed(self):
        return self._y

    @property
    def active_state_count(self):
        """Retained run records plus aggregates, bounded by 2 * width.

        This counts records rather than bytes; arbitrary-height area and y
        coordinates use Python integers. Per-call scratch is also O(width).
        """
        return len(self._runs) + len(self._stats)

    def _component(self, stats):
        if stats.area < self.min_area or (self.max_area is not None and stats.area > self.max_area):
            return None
        return Component((stats.x0, stats.y0, stats.x1, stats.y1),
                         "bright" if stats.bright else "dark", stats.area,
                         stats.peak, self.frame_id)

    def consume_row(self, signed, mask):
        if self._finished:
            raise RuntimeError("cannot consume rows after finish")
        for value, name, dtype in ((signed, "signed", np.int16), (mask, "mask", np.bool_)):
            if not isinstance(value, np.ndarray) or value.ndim != 1 or value.dtype != dtype:
                raise TypeError(f"{name} must be a one-dimensional {np.dtype(dtype).name} ndarray")
            if value.shape != (self.width,):
                raise ValueError(f"{name} shape must equal ({self.width},)")
        if np.any((signed < -255) | (signed > 255)):
            raise ValueError("signed must be in [-255,255]")
        if np.any(mask & (signed == 0)):
            raise ValueError("true mask pixels must have nonzero signed response")

        # The disjoint set lives for this row only. Closed labels and obsolete
        # union links cannot accumulate across a tall frame.
        stats = self._stats.copy()
        parent = list(range(len(stats)))
        rank = [0] * len(stats)

        def find(label):
            while parent[label] != label:
                parent[label] = parent[parent[label]]
                label = parent[label]
            return label

        def union(left, right):
            left, right = find(left), find(right)
            if left == right:
                return left
            if rank[left] < rank[right]:
                left, right = right, left
            parent[right] = left
            if rank[left] == rank[right]:
                rank[left] += 1
            a, b = stats[left], stats[right]
            # Sum each root exactly once, even when many runs bridge the same
            # two components or one component splits then joins again.
            stats[left] = _Stats(min(a.x0, b.x0), min(a.y0, b.y0),
                                 max(a.x1, b.x1), max(a.y1, b.y1),
                                 a.bright, a.area + b.area, max(a.peak, b.peak))
            return left

        current = []
        x = previous_start = 0
        while x < self.width:
            if not mask[x]:
                x += 1
                continue
            x0 = x
            bright = bool(signed[x] > 0)
            peak = abs(int(signed[x]))
            x += 1
            while x < self.width and mask[x] and bool(signed[x] > 0) == bright:
                peak = max(peak, abs(int(signed[x])))
                x += 1
            x1 = x
            label = len(parent)
            parent.append(label)
            rank.append(0)
            stats.append(_Stats(x0, self._y, x1, self._y + 1, bright, x1 - x0, peak))

            # Half-open runs touch diagonally when old.x1 == new.x0 or
            # old.x0 == new.x1; both equalities must remain eligible.
            while previous_start < len(self._runs) and self._runs[previous_start][1] < x0:
                previous_start += 1
            index = previous_start
            while index < len(self._runs) and self._runs[index][0] <= x1:
                old_label = self._runs[index][2]
                if stats[find(old_label)].bright == bright:
                    label = union(label, old_label)
                index += 1
            current.append((x0, x1, label))

        live = {find(label) for _, _, label in current}
        completed = []
        for label in range(len(stats)):
            if parent[label] == label and label not in live:
                component = self._component(stats[label])
                if component is not None:
                    completed.append(component)

        # Retain only live roots and densely remap the new frontier. Multiple
        # frontier runs may refer to the same aggregate.
        compact = {}
        next_stats = []
        next_runs = []
        for x0, x1, label in current:
            root = find(label)
            if root not in compact:
                compact[root] = len(next_stats)
                next_stats.append(stats[root])
            next_runs.append((x0, x1, compact[root]))
        self._runs, self._stats = next_runs, next_stats
        self._y += 1
        return completed

    def finish(self):
        if self._finished:
            return []
        completed = []
        for stats in self._stats:
            component = self._component(stats)
            if component is not None:
                completed.append(component)
        self._runs = []
        self._stats = []
        self._finished = True
        return completed
