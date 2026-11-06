"""Bounded per-stripe stitching; no full-frame array is retained here."""
from dataclasses import dataclass
import numpy as np
from strip_cache import StripCache,iter_tiles
from tile_runtime import StripeArena


@dataclass(frozen=True)
class ProcessedStripe:
    frame_id:object
    y0:int
    words:np.ndarray
    input_cache_high_water_bytes:int


def process_stripe(stripe,engine,radius,calibration=None,core_width=256,polarity=0,threshold=10):
    arena=StripeArena(stripe,calibration)
    result=np.empty((stripe.core_rows,stripe.width),np.uint32)
    for tile in iter_tiles(stripe,core_width):
        result[:,tile.core_x:tile.core_x+tile.core_width]=arena.run_tile(engine,tile,radius,polarity,threshold)
    return result


def process_chunks(chunks,width,total_rows,engine,*,radius=4,core_rows=64,core_width=256,
                   calibration=None,polarity=0,threshold=10,frame_id=0):
    """Yield exactly one completed immutable result stripe at a time.

    Generator suspension is backpressure to the source iterator. The caller
    must consume/release outputs instead of retaining a whole image list. This
    does not stop a non-backpressurable physical camera; upstream overflow must
    be reported and the frame discarded. No automatic retry after data loss.
    """
    cache=StripCache(width,total_rows,core_rows,radius,frame_id)
    if calibration is not None:
        if not isinstance(calibration,np.ndarray) or calibration.dtype!=np.uint32 or calibration.shape!=(width,):
            raise ValueError('calibration must be uint32 per-column data')
        calibration=calibration.copy() # one immutable calibration generation per frame
    def emit():
        source=cache.pop()
        result=process_stripe(source,engine,radius,calibration,core_width,polarity,threshold)
        result.flags.writeable=False
        return ProcessedStripe(frame_id,source.core_start,result,cache.high_water_bytes)
    for chunk in chunks:
        remaining=memoryview(chunk).cast('B')
        while remaining or cache.ready:
            if cache.ready:
                yield emit()
                continue
            count=cache.push(remaining)
            if not count: raise RuntimeError('producer made no progress')
            remaining=remaining[count:]
    cache.finish()
    while cache.ready: yield emit()
    if not cache.done: raise RuntimeError('incomplete stripe processing')
