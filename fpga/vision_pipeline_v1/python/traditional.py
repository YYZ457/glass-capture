"""Composable Mono8 CPU reference blocks; candidates are not defect decisions.

Only NumPy is required. Coordinates are (x0, y0, x1, y1), half-open.
No camera, model, FPGA synthesis, or accuracy assumptions are made here.
"""
from dataclasses import dataclass
from typing import Iterator, Optional, Tuple

import numpy as np

BBox = Tuple[int, int, int, int]


def _integer(value, name, minimum=0):
    if isinstance(value, (bool, np.bool_)) or not isinstance(value, (int, np.integer)):
        raise TypeError(f"{name} must be an integer")
    if value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return int(value)


def _array(value, name, dtype, shape=None):
    if not isinstance(value, np.ndarray) or value.dtype != dtype or value.ndim != 2:
        raise TypeError(f"{name} must be a two-dimensional {np.dtype(dtype).name} ndarray")
    if not all(value.shape):
        raise ValueError(f"{name} must be nonempty")
    if shape is not None and value.shape != shape:
        raise ValueError(f"{name} shape must equal {shape}")
    return value


def _valid(value, shape):
    return np.ones(shape, dtype=bool) if value is None else _array(value, "valid", np.bool_, shape)


@dataclass(frozen=True)
class CorrectionConfig:
    target_dn: int = 255
    min_denominator: int = 1
    invalid_fill: int = 0

    def __post_init__(self):
        for name in ("target_dn", "invalid_fill"):
            if _integer(getattr(self, name), name) > 255:
                raise ValueError(f"{name} must be <= 255")
        _integer(self.min_denominator, "min_denominator", 1)


@dataclass(frozen=True)
class CorrectionResult:
    image: np.ndarray
    valid: np.ndarray


def correct_dark_flat(image, dark=None, flat=None, config=CorrectionConfig()):
    """Return uint8 corrected data and bool valid mask without modifying inputs.

    dark/flat must be same-shaped uint8 calibration frames. With flat present:
    round-half-up(max(image-dark, 0) * target_dn / (flat-dark)), clipped to
    0..255. Missing dark means zero. Denominators below min_denominator are
    invalid and output invalid_fill. Dark-only subtraction saturates at zero;
    no calibration returns an independent copy. This is a numerical reference,
    not a calibration acquisition or exposure-normalization procedure.
    """
    _array(image, "image", np.uint8)
    if not isinstance(config, CorrectionConfig):
        raise TypeError("config must be CorrectionConfig")
    d = np.zeros(image.shape, np.int32) if dark is None else _array(dark, "dark", np.uint8, image.shape).astype(np.int32)
    signal = np.maximum(image.astype(np.int32) - d, 0)
    if flat is None:
        return CorrectionResult(signal.astype(np.uint8), np.ones(image.shape, bool))
    denominator = _array(flat, "flat", np.uint8, image.shape).astype(np.int32) - d
    valid = denominator >= config.min_denominator
    safe = np.where(valid, denominator, 1)
    corrected = (signal * config.target_dn + safe // 2) // safe
    corrected = np.clip(corrected, 0, 255).astype(np.uint8)
    corrected[~valid] = config.invalid_fill
    return CorrectionResult(corrected, valid)


@dataclass(frozen=True)
class BoxConfig:
    radius: int = 4
    polarity: str = "both"

    def __post_init__(self):
        _integer(self.radius, "radius")
        if self.polarity not in ("bright", "dark", "both"):
            raise ValueError("polarity must be bright, dark, or both")


@dataclass(frozen=True)
class ResponseResult:
    background: np.ndarray  # uint8 rounded local mean
    signed: np.ndarray      # int16 image-background; invalid locations are zero
    response: np.ndarray    # uint8 selected positive amplitude
    valid: np.ndarray       # bool: entire neighborhood is valid


def _box_sum(array, radius):
    """Exact separable int64 square sum with replicated image borders."""
    if radius == 0:
        return array.astype(np.int64)
    size = 2 * radius + 1
    p = np.pad(array, ((0, 0), (radius, radius)), mode="edge")
    c = np.pad(np.cumsum(p, axis=1, dtype=np.int64), ((0, 0), (1, 0)))
    horizontal = c[:, size:] - c[:, :-size]
    p = np.pad(horizontal, ((radius, radius), (0, 0)), mode="edge")
    c = np.pad(np.cumsum(p, axis=0, dtype=np.int64), ((1, 0), (0, 0)))
    return c[size:, :] - c[:-size, :]


def box_response(image, config=BoxConfig(), valid=None):
    """Subtract integer box mean; square width=2*radius+1, border=edge.

    Mean uses round-half-up once after exact summation. Invalid calibration
    pixels invalidate every output whose box touches them (conservative valid
    erosion); their signed/amplitude outputs are zero. Background remains the
    numerical mean of the supplied image even at invalid locations.
    """
    _array(image, "image", np.uint8)
    if not isinstance(config, BoxConfig):
        raise TypeError("config must be BoxConfig")
    mask = _valid(valid, image.shape)
    count = (2 * config.radius + 1) ** 2
    background = ((_box_sum(image, config.radius) + count // 2) // count).astype(np.uint8)
    output_valid = _box_sum(mask, config.radius) == count
    signed = image.astype(np.int16) - background.astype(np.int16)
    signed[~output_valid] = 0
    if config.polarity == "bright":
        response = np.maximum(signed, 0)
    elif config.polarity == "dark":
        response = np.maximum(-signed, 0)
    else:
        response = np.abs(signed)
    return ResponseResult(background, signed, response.astype(np.uint8), output_valid)


def threshold_mask(result, threshold_dn=10):
    """Inclusive amplitude >= threshold_dn (1..255) and valid pixels."""
    if not isinstance(result, ResponseResult):
        raise TypeError("result must be ResponseResult")
    _array(result.response, "response", np.uint8)
    _array(result.valid, "valid", np.bool_, result.response.shape)
    if _integer(threshold_dn, "threshold_dn", 1) > 255:
        raise ValueError("threshold_dn must be <= 255")
    return (result.response >= threshold_dn) & result.valid


@dataclass(frozen=True)
class ComponentConfig:
    min_area: int = 1
    max_area: Optional[int] = None
    max_candidates: int = 1024

    def __post_init__(self):
        _integer(self.min_area, "min_area", 1)
        if self.max_area is not None and _integer(self.max_area, "max_area", 1) < self.min_area:
            raise ValueError("max_area must be >= min_area")
        _integer(self.max_candidates, "max_candidates")


@dataclass(frozen=True)
class Candidate:
    bbox: BBox
    polarity: str
    area: int
    peak: int  # positive DN amplitude, not a probability


def connected_components(mask, signed_response, config=ComponentConfig()):
    """8-neighbor components, split by signed bright/dark polarity.

    True mask pixels must have nonzero int16 responses in [-255,255]. Sort by
    peak descending, area descending, y0,x0,y1,x1 ascending, then polarity;
    truncate only after all components are measured. Run globally on a stitched
    mask, not independently per tile, so boundary-spanning components merge.
    This Python flood-fill is a correctness/CPU reference, not a 16K throughput
    guarantee. max_candidates bounds returned count, not working memory/time.
    """
    _array(mask, "mask", np.bool_)
    _array(signed_response, "signed_response", np.int16, mask.shape)
    if not isinstance(config, ComponentConfig):
        raise TypeError("config must be ComponentConfig")
    if np.any((signed_response < -255) | (signed_response > 255)):
        raise ValueError("signed_response must be in [-255,255]")
    if np.any(mask & (signed_response == 0)):
        raise ValueError("true mask pixels must have nonzero signed_response")
    if config.max_candidates == 0:
        return []
    h, w = mask.shape
    visited = np.zeros(mask.shape, bool)
    found = []
    for y in range(h):
        for x_raw in np.flatnonzero(mask[y]):
            x = int(x_raw)
            if visited[y, x]:
                continue
            bright = signed_response[y, x] > 0
            stack = [(y, x)]
            visited[y, x] = True
            x0 = x1 = x
            y0 = y1 = y
            area = peak = 0
            while stack:
                cy, cx = stack.pop()
                area += 1
                peak = max(peak, abs(int(signed_response[cy, cx])))
                x0, x1 = min(x0, cx), max(x1, cx)
                y0, y1 = min(y0, cy), max(y1, cy)
                for ny in range(max(0, cy - 1), min(h, cy + 2)):
                    for nx in range(max(0, cx - 1), min(w, cx + 2)):
                        if mask[ny, nx] and not visited[ny, nx] and (signed_response[ny, nx] > 0) == bright:
                            visited[ny, nx] = True
                            stack.append((ny, nx))
            if area >= config.min_area and (config.max_area is None or area <= config.max_area):
                found.append(Candidate((x0, y0, x1 + 1, y1 + 1), "bright" if bright else "dark", area, peak))
    found.sort(key=lambda c: (-c.peak, -c.area, c.bbox[1], c.bbox[0], c.bbox[3], c.bbox[2], c.polarity))
    return found[:config.max_candidates]


def _bbox(bbox, shape):
    if not isinstance(bbox, (tuple, list)) or len(bbox) != 4:
        raise TypeError("bbox must be a four-element tuple/list")
    x0, y0, x1, y1 = (_integer(v, "bbox coordinate") for v in bbox)
    if not (x0 < x1 <= shape[1] and y0 < y1 <= shape[0]):
        raise ValueError("bbox must be nonempty and inside the image")
    return x0, y0, x1, y1


def resize_nearest(image, output_shape):
    """Top-left/floor nearest mapping: src=floor(dst*src_size/dst_size).

    output_shape is (height,width). No interpolation, normalization, or padding.
    """
    _array(image, "image", np.uint8)
    if not isinstance(output_shape, (tuple, list)) or len(output_shape) != 2:
        raise TypeError("output_shape must be (height,width)")
    oh, ow = (_integer(v, "output dimension", 1) for v in output_shape)
    yy = np.arange(oh, dtype=np.int64) * image.shape[0] // oh
    xx = np.arange(ow, dtype=np.int64) * image.shape[1] // ow
    return image[yy[:, None], xx[None, :]].copy()


def crop_roi(image, bbox, output_shape=None):
    """Strict in-bounds half-open crop, optionally resize_nearest; returns copy.

    Original bbox remains in source coordinates; caller retains it for mapping
    future model outputs. Invalid/out-of-frame coordinates are never clipped.
    """
    _array(image, "image", np.uint8)
    x0, y0, x1, y1 = _bbox(bbox, image.shape)
    cropped = image[y0:y1, x0:x1]
    return cropped.copy() if output_shape is None else resize_nearest(cropped, output_shape)


@dataclass(frozen=True)
class Tile:
    read_bbox: BBox  # global input extent including clipped halo
    core_bbox: BBox  # global output extent, disjoint and covers whole image
    core_in_read: BBox  # same output extent in local tile coordinates


def iter_tiles(shape, core_shape=(1024, 1024), halo=4) -> Iterator[Tile]:
    """Yield row-major tiles; shape/core_shape are (height,width).

    Halo is clipped only at actual image boundaries. Each global pixel belongs
    to exactly one valid core; read extents overlap. Core edges do not discard
    candidates. Neighborhood operators require halo >= neighborhood radius.
    """
    dims = []
    for value, name in ((shape, "shape"), (core_shape, "core_shape")):
        if not isinstance(value, (tuple, list)) or len(value) != 2:
            raise TypeError(f"{name} must be (height,width)")
        dims.append(tuple(_integer(v, name, 1) for v in value))
    h, w = dims[0]
    ch, cw = dims[1]
    halo = _integer(halo, "halo")
    for y0 in range(0, h, ch):
        for x0 in range(0, w, cw):
            x1, y1 = min(w, x0 + cw), min(h, y0 + ch)
            rx0, ry0 = max(0, x0 - halo), max(0, y0 - halo)
            rx1, ry1 = min(w, x1 + halo), min(h, y1 + halo)
            yield Tile((rx0, ry0, rx1, ry1), (x0, y0, x1, y1), (x0-rx0, y0-ry0, x1-rx0, y1-ry0))


def tiled_box_response(image, config=BoxConfig(), valid=None, core_shape=(1024, 1024), halo=None):
    """Exactly box_response, using bounded tile intermediates and full outputs.

    Set halo>=radius (default radius). Stitch cores before thresholding/global
    connected_components. Returned full-size arrays still require RAM; this
    is not an out-of-core image reader or streaming connected-components core.
    """
    _array(image, "image", np.uint8)
    if not isinstance(config, BoxConfig):
        raise TypeError("config must be BoxConfig")
    mask = _valid(valid, image.shape)
    halo = config.radius if halo is None else _integer(halo, "halo")
    if halo < config.radius:
        raise ValueError("halo must be >= radius")
    background = np.empty(image.shape, np.uint8)
    signed = np.empty(image.shape, np.int16)
    response = np.empty(image.shape, np.uint8)
    output_valid = np.empty(image.shape, bool)
    for tile in iter_tiles(image.shape, core_shape, halo):
        rx0, ry0, rx1, ry1 = tile.read_bbox
        x0, y0, x1, y1 = tile.core_bbox
        lx0, ly0, lx1, ly1 = tile.core_in_read
        part = box_response(image[ry0:ry1, rx0:rx1], config, mask[ry0:ry1, rx0:rx1])
        for dest, source in ((background, part.background), (signed, part.signed), (response, part.response), (output_valid, part.valid)):
            dest[y0:y1, x0:x1] = source[ly0:ly1, lx0:lx1]
    return ResponseResult(background, signed, response, output_valid)
