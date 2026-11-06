"""Host-side composition and ap_ctrl_hs lifecycle. No /dev/mem backend.

NativeEngine runs the exact synthesizable C++ on CPU. HlsExecutor requires a
board-specific Backend that owns a coherent DMA allocation, MMIO barriers,
cache synchronization and a proven reset-and-drain implementation.
"""
from dataclasses import dataclass, fields
import ctypes
import math
import threading
import time
from typing import Protocol
import numpy as np


REGISTERS = dict(zip(
    ('memory_words opcode input_offset input2_offset output_offset weights_offset '
     'bias_offset multiplier_offset shift_offset channels_in height width '
     'channels_out kernel stride pad flags').split(), range(0x1c, 0xa4, 8)))
CONTROL, MEMORY_LOW, MEMORY_HIGH, STATUS = 0, 0x10, 0x14, 0xa4
CONV, MAXPOOL, REQUANT, ADD_SAT = 1, 2, 3, 4
DEPTHWISE, RELU, BIAS = 1, 2, 4


@dataclass(frozen=True)
class Job:
    opcode: int
    input_offset: int
    output_offset: int
    channels_in: int
    height: int
    width: int
    input2_offset: int = 0
    weights_offset: int = 0
    bias_offset: int = 0
    multiplier_offset: int = 0
    shift_offset: int = 0
    channels_out: int = 0
    kernel: int = 0
    stride: int = 0
    pad: int = 0
    flags: int = 0

    def scalars(self, memory_words):
        values = {'memory_words': memory_words}
        values.update({f.name: getattr(self, f.name) for f in fields(self)})
        for name, value in values.items():
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xffffffff:
                raise ValueError(f'{name} must be a uint32 integer')
        return [values[name] for name in REGISTERS]


class Arena:
    """CPU arena; returned offsets are words. Not a physical DMA allocation."""
    def __init__(self, words=1 << 20):
        if isinstance(words, bool) or not isinstance(words, int) or not 1 <= words <= 1 << 24:
            raise ValueError('arena size must be 1..2^24 words')
        self.memory = np.zeros(words, dtype=np.uint32)
        self.next_word = 0

    def allocate(self, count):
        if isinstance(count, bool) or not isinstance(count, int) or count <= 0:
            raise ValueError('positive element count required')
        offset = (self.next_word + 15) & ~15
        if offset + count > self.memory.size:
            raise MemoryError('arena exhausted')
        self.next_word = offset + count
        return offset

    def put(self, tensor):
        a = np.asarray(tensor)
        if a.dtype not in (np.dtype('int8'), np.dtype('int32'), np.dtype('uint32')) or not a.size:
            raise ValueError('put accepts nonempty int8/int32/uint32 arrays')
        offset = self.allocate(int(a.size))
        if a.dtype == np.int8:
            data = a.view(np.uint8).astype(np.uint32)
        elif a.dtype == np.int32:
            data = a.view(np.uint32)
        else:
            data = a
        self.memory[offset:offset+a.size] = data.reshape(-1)
        return offset

    def get(self, offset, shape, dtype):
        if not shape or any(type(n) is not int or n <= 0 for n in shape):
            raise ValueError('positive integer shape required')
        n = math.prod(shape)
        if type(offset) is not int or offset < 0 or offset+n > self.next_word:
            raise ValueError('range outside allocated arena')
        words = self.memory[offset:offset+n]
        if dtype == 'int8':
            result = (words & 255).astype(np.uint8).view(np.int8)
        elif dtype == 'int32':
            result = words.view(np.int32).copy()
        else:
            raise ValueError('output dtype must be int8 or int32')
        return result.reshape(shape)


class OperatorError(RuntimeError):
    def __init__(self, status):
        self.status = status
        super().__init__(f'quant_engine rejected job, status={status}')


class NativeEngine:
    def __init__(self, library):
        self.lib = ctypes.CDLL(str(library))
        self.call = self.lib.vision_quant_engine
        self.call.argtypes = [ctypes.POINTER(ctypes.c_uint32)] + [ctypes.c_uint32]*17
        self.call.restype = ctypes.c_uint32

    def run(self, arena, job):
        status = self.call(arena.memory.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                           *job.scalars(int(arena.memory.size)))
        if status:
            raise OperatorError(status)


class Backend(Protocol):
    """All methods synchronous. Sole owner, no competing MMIO/DMA accesses.

    dma_address is a DEVICE DMA/IOVA address, never a Python/CPU pointer.
    sync_for_device publishes all input+descriptor writes before start;
    sync_for_cpu waits/orders device writes before CPU reads. A timeout never
    releases an allocation. reset_and_drain returns True ONLY after bus masters
    and outstanding writes have stopped; merely clearing ap_start is inadequate.
    """
    dma_address: int
    memory_words: int
    def read32(self, offset: int) -> int: ...
    def write32(self, offset: int, value: int) -> None: ...
    def sync_for_device(self) -> None: ...
    def sync_for_cpu(self) -> None: ...
    def reset_and_drain(self) -> bool: ...


class HlsExecutor:
    def __init__(self, backend: Backend, clock=time.monotonic, sleep=time.sleep):
        self.backend, self.clock, self.sleep = backend, clock, sleep
        self.quarantined = False
        self.lock = threading.Lock()

    def run(self, job: Job, timeout_s=1.0, poll_s=0.0001):
        if not math.isfinite(timeout_s) or timeout_s <= 0 or not math.isfinite(poll_s) or poll_s < 0:
            raise ValueError('finite positive timeout and nonnegative poll interval required')
        with self.lock:
            if self.quarantined:
                raise RuntimeError('engine quarantined: recover before reusing allocation')
            b = self.backend
            if type(b.memory_words) is not int or not 1 <= b.memory_words <= 1 << 24:
                raise ValueError('invalid DMA allocation size')
            if type(b.dma_address) is not int or not 0 <= b.dma_address < (1 << 64) or b.dma_address % 4:
                raise ValueError('aligned 64-bit device DMA address required')
            if b.dma_address + 4*b.memory_words > 1 << 64:
                raise ValueError('DMA range wraps')
            scalars = job.scalars(b.memory_words)
            ctrl = b.read32(CONTROL)  # clear stale ap_done before a new job
            if ctrl & 0x81 or not ctrl & 4:
                raise RuntimeError('engine not idle or auto-restart enabled')
            b.write32(MEMORY_LOW, b.dma_address & 0xffffffff)
            b.write32(MEMORY_HIGH, b.dma_address >> 32)
            for offset, value in zip(REGISTERS.values(), scalars):
                b.write32(offset, value)
            b.sync_for_device()
            deadline = self.clock()+timeout_s
            # From here, even a failed MMIO write may have reached the hardware.
            # On ANY exception retain allocation and require reset/drain.
            try:
                b.write32(CONTROL, 1)  # ap_ctrl_hs, auto_restart=0
                while True:
                    ctrl = b.read32(CONTROL)
                    if ctrl & 2:  # ap_done is clear-on-read; remember this read
                        status = b.read32(STATUS)
                        b.sync_for_cpu()
                        break
                    if self.clock() >= deadline:
                        raise TimeoutError('accelerator deadline; buffers quarantined')
                    self.sleep(poll_s)
            except BaseException:
                self.quarantined = True
                raise
            if status:
                raise OperatorError(status)

    def recover(self):
        with self.lock:
            if not self.quarantined:
                return
            if self.backend.reset_and_drain() is not True:
                raise RuntimeError('reset/drain not confirmed; retain DMA allocation')
            ctrl = self.backend.read32(CONTROL)
            if ctrl & 0x81 or not ctrl & 4:
                raise RuntimeError('engine still not idle')
            self.quarantined = False
