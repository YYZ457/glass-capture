"""Tested ap_ctrl_hs lifecycle for the v2 map; board backend is injected.

No /dev/mem or guessed DMA mapping. Backend exclusively owns the accelerator
and allocation. sync methods must include cache and MMIO ordering. DMA address
is the address mapped for THIS PL master, not an SG3000 PCIe device IOVA.
"""
import math,threading,time
from tile_runtime import REGISTERS,RETURN_STATUS,TileRejected


class TileExecutor:
    def __init__(self,backend,clock=time.monotonic,sleep=time.sleep):
        self.backend,self.clock,self.sleep=backend,clock,sleep
        self.quarantined=False
        self.lock=threading.Lock()

    def run(self,job,timeout_s=1.0,poll_s=0.0001):
        if not math.isfinite(timeout_s) or timeout_s<=0 or not math.isfinite(poll_s) or poll_s<0:
            raise ValueError('finite positive timeout and nonnegative polling period required')
        with self.lock:
            if self.quarantined: raise RuntimeError('allocation quarantined; reset/drain required')
            b=self.backend
            if type(b.memory_words) is not int or not 1<=b.memory_words<=1<<29:
                raise ValueError('invalid allocation size')
            if type(b.dma_address) is not int or b.dma_address<0 or b.dma_address%4 or b.dma_address+4*b.memory_words>1<<64:
                raise ValueError('aligned device DMA address and nonwrapping allocation required')
            values=job.scalars(b.memory_words)
            ctrl=b.read32(0) # clear stale done
            if ctrl&0x81 or not ctrl&4: raise RuntimeError('not idle or auto_restart set')
            b.write32(0x10,b.dma_address&0xffffffff);b.write32(0x14,b.dma_address>>32)
            for reg,value in zip(REGISTERS.values(),values): b.write32(reg,value)
            b.sync_for_device()
            deadline=self.clock()+timeout_s
            try:
                b.write32(0,1)
                while True:
                    ctrl=b.read32(0)
                    if ctrl&2:
                        status=b.read32(RETURN_STATUS)
                        b.sync_for_cpu()
                        break
                    if self.clock()>=deadline: raise TimeoutError('retain all DMA buffers until reset and drain')
                    self.sleep(poll_s)
            except BaseException:
                self.quarantined=True
                raise
            if status: raise TileRejected(status)

    def recover(self):
        """Backend must confirm master stopped AND outstanding writes drained.

        idle alone or clearing ap_start is not cancellation. On failure leave
        all buffers allocated and quarantined, including input/calibration.
        """
        with self.lock:
            if not self.quarantined:return
            if self.backend.reset_and_drain() is not True: raise RuntimeError('drain not confirmed')
            ctrl=self.backend.read32(0)
            if ctrl&0x81 or not ctrl&4: raise RuntimeError('not idle after reset')
            self.quarantined=False
