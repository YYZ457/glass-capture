"""Bounded Mono8 producer cache for full-width, halo-bearing FPGA stripes.

The caller keeps ownership of input fragments. ``push`` copies only its returned
byte count; resubmit the unconsumed suffix after draining ``pop``. A ready stripe
applies backpressure: no input is consumed until it is popped. ``pop`` creates an
immutable, caller-owned snapshot, so later DMA/cache activity cannot overwrite it.

``buffered_bytes`` and ``high_water_bytes`` count retained row payload and partial
row bytes, excluding caller input, emitted snapshots, Python object overhead and
temporary copies during snapshot/row creation. Retained payload never exceeds
``width * (core_rows + 2 * radius)``. No full-frame accumulation is performed.

Each emitted source rectangle has the full frame width. Its vertical bounds are
clipped to the real frame; the HLS kernel replicates only the *global* frame edges.
Interior stripe edges already contain the true neighbouring rows. Horizontal
tiles therefore share the same source buffer and keep their actual frame x.
"""

from collections import deque
from dataclasses import dataclass
from typing import Iterator, Optional, Union


FrameId = Optional[Union[int, str]]


def _integer(name: str, value: int, low: int, high: Optional[int] = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < low or (high is not None and value > high):
        bound = f"{low}..{high}" if high is not None else f">= {low}"
        raise ValueError(f"{name} must be {bound}")
    return value


@dataclass(frozen=True)
class Stripe:
    """Immutable full-width source pixels plus the disjoint core's location."""

    data: bytes
    width: int
    source_y: int
    core_y: int
    core_rows: int
    frame_id: FrameId
    core_start: int
    frame_rows: Optional[int] = None
    halo_radius: Optional[int] = None

    def __post_init__(self) -> None:
        if not isinstance(self.data, bytes):
            raise TypeError("Stripe.data must be immutable bytes")
        _integer("width", self.width, 1, 16384)
        _integer("source_y", self.source_y, 0)
        _integer("core_y", self.core_y, 0)
        _integer("core_rows", self.core_rows, 1, 64)
        _integer("core_start", self.core_start, 0)
        if len(self.data) % self.width:
            raise ValueError("Stripe.data must contain complete Mono8 rows")
        if self.core_y + self.core_rows > self.source_rows:
            raise ValueError("core extends beyond source rows")
        if self.core_start != self.source_y + self.core_y:
            raise ValueError("core_start must equal source_y + core_y")
        if (self.frame_rows is None) != (self.halo_radius is None):
            raise ValueError("frame_rows and halo_radius must be supplied together")
        if self.frame_rows is not None:
            _integer("frame_rows", self.frame_rows, 1, 65536)
            _integer("halo_radius", self.halo_radius, 0, 7)
            if self.core_start + self.core_rows > self.frame_rows:
                raise ValueError("core extends beyond frame")
            if self.source_y != max(0,self.core_start-self.halo_radius) or self.source_y+self.source_rows != min(self.frame_rows,self.core_start+self.core_rows+self.halo_radius):
                raise ValueError("source rectangle does not match declared halo and frame edges")

    @property
    def source_rows(self) -> int:
        return len(self.data) // self.width

    @property
    def source_height(self) -> int:
        """Height passed to the HLS source descriptor."""
        return self.source_rows


@dataclass(frozen=True)
class TileDescriptor:
    """Core coordinates are local to Stripe.data; global_y uses Python ints."""

    core_x: int
    core_y: int
    core_width: int
    core_height: int
    global_y: int
    frame_id: FrameId


def iter_tiles(stripe: Stripe, core_width: int = 256) -> Iterator[TileDescriptor]:
    """Partition the stripe core exactly once; do not crop its source buffer."""
    _integer("core_width", core_width, 1, 256)
    for x in range(0, stripe.width, core_width):
        yield TileDescriptor(
            core_x=x,
            core_y=stripe.core_y,
            core_width=min(core_width, stripe.width - x),
            core_height=stripe.core_rows,
            global_y=stripe.source_y + stripe.core_y,
            frame_id=stripe.frame_id,
        )


class StripCache:
    """One-frame streaming cache with bounded storage and explicit backpressure.

    ``finish`` validates that all frame bytes have arrived, even if the final
    stripe still awaits ``pop``. A premature finish raises without closing the
    cache, allowing the producer to supply the missing bytes. Non-empty excess
    input is rejected atomically, including while a stripe is ready.
    """

    def __init__(self, width: int, total_rows: int, core_rows: int = 64,
                 radius: int = 7, frame_id: FrameId = None) -> None:
        self.width = _integer("width", width, 1, 16384)
        self.total_rows = _integer("total_rows", total_rows, 1, 65536)
        self.core_rows = _integer("core_rows", core_rows, 1, 64)
        self.radius = _integer("radius", radius, 0, 7)
        self.frame_id = frame_id
        self._rows = deque()
        self._partial = bytearray()
        self._row_start = 0
        self._rows_received = 0
        self._bytes_received = 0
        self._next_core_start = 0
        self._ready = False
        self._finished = False
        self._high_water = 0

    @property
    def ready(self) -> bool:
        return self._ready

    @property
    def buffered_bytes(self) -> int:
        return len(self._rows) * self.width + len(self._partial)

    @property
    def high_water_bytes(self) -> int:
        return self._high_water

    @property
    def max_buffered_bytes(self) -> int:
        return self.width * (self.core_rows + 2 * self.radius)

    @property
    def bytes_received(self) -> int:
        return self._bytes_received

    @property
    def done(self) -> bool:
        """All frame core stripes have been popped (independent of finish)."""
        return self._next_core_start >= self.total_rows

    def _required_end(self) -> int:
        return min(self.total_rows,
                   self._next_core_start + self.core_rows + self.radius)

    def _update_ready(self) -> None:
        self._ready = (not self.done and self._rows_received >= self._required_end())

    def push(self, fragment: object) -> int:
        """Consume a bounded prefix of any contiguous bytes-like fragment.

        The producer must use the returned count, including zero. Strided views
        are rejected because flattening them would eagerly copy caller storage.
        """
        try:
            view = memoryview(fragment)
        except TypeError as exc:
            raise TypeError("fragment must support the buffer protocol") from exc
        if not view.c_contiguous:
            raise ValueError("fragment must be C-contiguous")
        view = view.cast("B")
        size = len(view)
        remaining = self.width * self.total_rows - self._bytes_received
        if size > remaining:
            raise ValueError("input exceeds declared frame byte count")
        if not size:
            return 0
        if self._finished:
            raise ValueError("cannot push after finish")
        if self._ready:
            return 0
        # Stop exactly at the next halo-complete boundary, even for a huge chunk.
        take = min(size, self._required_end() * self.width - self._bytes_received)
        consumed = 0
        while consumed < take:
            count = min(self.width - len(self._partial), take - consumed)
            self._partial.extend(view[consumed:consumed + count])
            consumed += count
            self._bytes_received += count
            self._high_water = max(self._high_water, self.buffered_bytes)
            if len(self._partial) == self.width:
                self._rows.append(bytes(self._partial))
                self._partial.clear()
                self._rows_received += 1
        self._update_ready()
        return consumed

    def pop(self) -> Stripe:
        """Transfer a copied stripe snapshot and retain only future halo rows."""
        if not self._ready:
            raise LookupError("no halo-complete stripe is ready")
        core_start = self._next_core_start
        count = min(self.core_rows, self.total_rows - core_start)
        source_start = max(0, core_start - self.radius)
        source_end = min(self.total_rows, core_start + count + self.radius)
        # _rows starts exactly at the requested source start after each eviction.
        assert self._row_start == source_start
        # At EOF several overlapping small cores may already be ready. Slice by
        # row count, never accidentally include surplus rows in their snapshots.
        source_count = source_end - source_start
        data = b"".join(row for i, row in enumerate(self._rows) if i < source_count)
        stripe = Stripe(data=data, width=self.width, source_y=source_start,
                        core_y=core_start - source_start, core_rows=count,
                        frame_id=self.frame_id, core_start=core_start,
                        frame_rows=self.total_rows,halo_radius=self.radius)
        self._next_core_start += count
        keep_start = (self.total_rows if self.done else
                      max(0, self._next_core_start - self.radius))
        while self._rows and self._row_start < keep_start:
            self._rows.popleft()
            self._row_start += 1
        self._update_ready()
        return stripe

    def finish(self) -> None:
        """Validate complete input; drain all remaining ready stripes afterward."""
        expected = self.width * self.total_rows
        if self._bytes_received != expected:
            suffix = " (partial row)" if self._partial else ""
            raise ValueError(f"truncated frame: received {self._bytes_received} of "
                             f"{expected} bytes{suffix}")
        self._finished = True
