import os
import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from quant_runtime import *


class FakeBackend:
    dma_address = 0x1234567800
    memory_words = 4096
    def __init__(self, finish=True, status=0):
        self.events=[]; self.started=False; self.finish=finish; self.status=status
        self.t=0; self.drained=False; self.initial=6; self.fail_write=False
    def read32(self, offset):
        self.events.append(('read',offset))
        if offset==CONTROL:
            return (6 if self.finish else 0) if self.started else self.initial
        if offset==STATUS: return self.status
        raise AssertionError('unexpected read')
    def write32(self, offset, value):
        self.events.append(('write',offset,value))
        if offset==CONTROL:
            self.started=True
            if self.fail_write: raise OSError('bus failure after write')
    def sync_for_device(self): self.events.append(('device',))
    def sync_for_cpu(self): self.events.append(('cpu',))
    def reset_and_drain(self):
        self.events.append(('reset',))
        if self.drained: self.started=False; self.initial=6
        return self.drained
    def clock(self): return self.t
    def sleep(self, seconds): self.t += max(seconds,0.01)


class RuntimeTests(unittest.TestCase):
    def job(self): return Job(ADD_SAT,0,64,1,1,8,input2_offset=32)
    def runner(self,b): return HlsExecutor(b, b.clock, b.sleep)
    def test_map(self):
        self.assertEqual(len(REGISTERS),17)
        self.assertEqual(REGISTERS['flags'],0x9c)
        self.assertEqual(REGISTERS['output_offset'],0x3c)
    def test_order_address_and_stale_done(self):
        b=FakeBackend(); r=self.runner(b); r.run(self.job()); r.run(self.job())
        ev=b.events
        self.assertEqual(ev[0],('read',CONTROL))
        self.assertIn(('write',MEMORY_HIGH,0x12),ev)
        self.assertIn(('write',MEMORY_LOW,0x34567800),ev)
        self.assertLess(ev.index(('device',)),ev.index(('write',CONTROL,1)))
        self.assertGreater(ev.index(('cpu',)),ev.index(('read',STATUS)))
        self.assertFalse(r.quarantined)
    def test_refuse_busy_autorestart(self):
        for control in (0,1,0x84,0x85):
            b=FakeBackend(); b.initial=control
            with self.assertRaises(RuntimeError): self.runner(b).run(self.job())
            self.assertFalse(any(x[0]=='write' for x in b.events))
    def test_timeout_quarantine_recovery(self):
        b=FakeBackend(finish=False); r=self.runner(b)
        with self.assertRaises(TimeoutError): r.run(self.job(),timeout_s=.025)
        self.assertTrue(r.quarantined); self.assertNotIn(('cpu',),b.events)
        before=list(b.events)
        with self.assertRaises(RuntimeError): r.run(self.job())
        self.assertEqual(before,b.events)
        with self.assertRaises(RuntimeError): r.recover()
        self.assertTrue(r.quarantined)
        b.drained=True; r.recover(); b.finish=True; r.run(self.job())
        self.assertFalse(r.quarantined)
    def test_uncertain_start_is_quarantined(self):
        b=FakeBackend(); b.fail_write=True; r=self.runner(b)
        with self.assertRaises(OSError): r.run(self.job())
        self.assertTrue(r.quarantined)
    def test_operator_rejection_not_hardware_hang(self):
        b=FakeBackend(status=7); r=self.runner(b)
        with self.assertRaises(OperatorError) as e: r.run(self.job())
        self.assertEqual(e.exception.status,7); self.assertFalse(r.quarantined)
        self.assertIn(('cpu',),b.events)
    def test_invalid_inputs_no_start(self):
        for timeout in (0,-1,float('nan'),float('inf')):
            b=FakeBackend()
            with self.assertRaises(ValueError): self.runner(b).run(self.job(),timeout)
            self.assertFalse(b.events)
        for address in (-4,1,1<<64,(1<<64)-4):
            b=FakeBackend(); b.dma_address=address
            with self.assertRaises(ValueError): self.runner(b).run(self.job())
            self.assertFalse(b.events)
        b=FakeBackend()
        with self.assertRaises(ValueError): self.runner(b).run(Job(4,-1,64,1,1,1))
        self.assertFalse(b.events)
    def test_arena(self):
        a=Arena(128); x=np.array([-128,-1,0,127],dtype=np.int8)
        off=a.put(x); np.testing.assert_array_equal(a.get(off,(4,),'int8'),x)
        y=np.array([-2147483648,-1,2147483647],dtype=np.int32)
        off=a.put(y); self.assertEqual(off%16,0)
        np.testing.assert_array_equal(a.get(off,(3,),'int32'),y)
        with self.assertRaises(MemoryError): a.allocate(129)
        with self.assertRaises(ValueError): a.put(np.zeros(2,dtype=np.float32))


@unittest.skipUnless(os.environ.get('VISION_NATIVE_LIBRARY'),'native library not configured')
class NativeComposition(unittest.TestCase):
    def test_conv_requant_pool_repeat(self):
        engine=NativeEngine(os.environ['VISION_NATIVE_LIBRARY'])
        for seed in range(5):
            rng=np.random.default_rng(seed); a=Arena(4096)
            x=rng.integers(-80,80,(1,8,8),dtype=np.int8)
            inp=a.put(x); w=a.put(np.array([2],dtype=np.int8)); acc=a.allocate(64)
            q=a.allocate(64); pool=a.allocate(16)
            mult=a.put(np.array([1],dtype=np.uint32)); shift=a.put(np.array([1],dtype=np.uint32))
            engine.run(a,Job(CONV,inp,acc,1,8,8,weights_offset=w,channels_out=1,kernel=1,stride=1))
            np.testing.assert_array_equal(a.get(acc,(1,8,8),'int32'),x.astype(np.int32)*2)
            engine.run(a,Job(REQUANT,acc,q,1,8,8,multiplier_offset=mult,shift_offset=shift))
            np.testing.assert_array_equal(a.get(q,(1,8,8),'int8'),x)
            engine.run(a,Job(MAXPOOL,q,pool,1,8,8))
            expected=x.reshape(1,4,2,4,2).max(axis=(2,4))
            np.testing.assert_array_equal(a.get(pool,(1,4,4),'int8'),expected)
            before=a.memory.copy()
            with self.assertRaises(OperatorError): engine.run(a,Job(ADD_SAT,inp,inp,1,8,8,input2_offset=q))
            np.testing.assert_array_equal(a.memory,before)


if __name__=='__main__': unittest.main()
