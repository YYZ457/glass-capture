"""Explicit tile jobs and a native backend using the SAME synthesizable C++.

Packed Mono8 input here is a separate ABI from v1 quant_engine's one-word INT8.
Offsets are words; stride is bytes. No CPU pointer is ever a device DMA address.
"""
from dataclasses import dataclass,fields
import ctypes
import numpy as np

REGISTERS=dict(zip(('memory_words input_offset stride_bytes image_width image_height '
                   'core_x core_y core_width core_height radius calibration_offset '
                   'output_offset polarity threshold flags').split(),range(0x1c,0x94,8)))
RETURN_STATUS=0x94


@dataclass(frozen=True)
class TileJob:
    input_offset:int
    stride_bytes:int
    image_width:int
    image_height:int
    core_x:int
    core_y:int
    core_width:int
    core_height:int
    radius:int
    calibration_offset:int
    output_offset:int
    polarity:int=0
    threshold:int=10
    flags:int=0

    def scalars(self,memory_words):
        values={'memory_words':memory_words,**{f.name:getattr(self,f.name) for f in fields(self)}}
        if any(type(v) is not int or not 0<=v<=0xffffffff for v in values.values()):
            raise ValueError('job scalars must be uint32 Python integers')
        return [values[k] for k in REGISTERS]


class TileRejected(RuntimeError):
    def __init__(self,status):
        self.status=status
        super().__init__(f'preprocess_tile rejected job: {status}')


class NativeEngine:
    def __init__(self,path):
        self.library=ctypes.CDLL(str(path))
        self.call=self.library.vision_preprocess_tile
        self.call.argtypes=[ctypes.POINTER(ctypes.c_uint32)]+[ctypes.c_uint32]*15
        self.call.restype=ctypes.c_uint32

    def run(self,memory,job):
        if not isinstance(memory,np.ndarray) or memory.dtype!=np.uint32 or memory.ndim!=1 or not memory.flags.c_contiguous:
            raise ValueError('contiguous uint32 arena required')
        status=self.call(memory.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),*job.scalars(int(memory.size)))
        if status: raise TileRejected(status)


class StripeArena:
    """Bounded source stripe + calibration + reusable ONE-tile output staging.

    CPU memory only. Hardware adapters copy these bytes into owned DMA memory.
    run_tile returns a copy so subsequent tile reuse cannot mutate prior output.
    """
    def __init__(self,stripe,calibration=None):
        width,rows=stripe.width,stripe.source_rows
        if not 1<=width<=16384 or not 1<=rows<=78: raise ValueError('invalid bounded stripe shape')
        if stripe.frame_rows is None or stripe.halo_radius is None:
            raise ValueError('executable stripe requires declared frame boundary and halo metadata')
        self.stride=(width+3)&~3
        padded=np.full((rows,self.stride),0xa5,np.uint8)
        padded[:,:width]=np.frombuffer(stripe.data,np.uint8).reshape(rows,width)
        source=padded.reshape(-1).view('<u4')
        self.input_offset=16
        self.calibration_offset=(self.input_offset+source.size+31)&~15
        self.output_offset=(self.calibration_offset+width+31)&~15
        self.memory=np.full(self.output_offset+256*64+16,0xdeadbeef,np.uint32)
        self.memory[self.input_offset:self.input_offset+source.size]=source
        if calibration is not None:
            if not isinstance(calibration,np.ndarray) or calibration.dtype!=np.uint32 or calibration.shape!=(width,):
                raise ValueError('one uint32 calibration word per source column required')
            self.memory[self.calibration_offset:self.calibration_offset+width]=calibration
        self.calibration_enabled=calibration is not None
        self.stripe=stripe

    def job(self,tile,radius,polarity=0,threshold=10):
        if type(radius) is not int or not 0<=radius<=self.stripe.halo_radius:
            raise ValueError('requested filter radius exceeds the supplied halo')
        if tile.core_y!=self.stripe.core_y or tile.core_height!=self.stripe.core_rows or tile.global_y!=self.stripe.core_start or tile.frame_id!=self.stripe.frame_id:
            raise ValueError('tile descriptor belongs to a different stripe core')
        if type(tile.core_x) is not int or type(tile.core_width) is not int or not (0<=tile.core_x and 1<=tile.core_width<=256 and tile.core_x+tile.core_width<=self.stripe.width):
            raise ValueError('invalid horizontal core extent')
        return TileJob(self.input_offset,self.stride,self.stripe.width,self.stripe.source_rows,
                       tile.core_x,tile.core_y,tile.core_width,tile.core_height,radius,
                       self.calibration_offset,self.output_offset,polarity,threshold,
                       int(self.calibration_enabled))

    def run_tile(self,engine,tile,radius,polarity=0,threshold=10):
        job=self.job(tile,radius,polarity,threshold)
        engine.run(self.memory,job)
        n=tile.core_width*tile.core_height
        return self.memory[self.output_offset:self.output_offset+n].reshape(tile.core_height,tile.core_width).copy()
