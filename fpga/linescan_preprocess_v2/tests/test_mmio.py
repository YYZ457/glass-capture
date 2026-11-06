import sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from tile_runtime import TileJob,TileRejected
from mmio_runtime import TileExecutor

class Bus:
    memory_words=65536;dma_address=0x1234567800
    def __init__(self):
        self.events=[];self.initial=6;self.started=False;self.done=True;self.status=0
        self.t=0;self.drained=False;self.fail_start=False
    def read32(self,a):
        self.events.append(('r',a))
        if a==0:return self.initial if not self.started else (6 if self.done else 0)
        assert a==0x94
        return self.status
    def write32(self,a,v):
        self.events.append(('w',a,v))
        if a==0:
            self.started=True
            if self.fail_start:raise OSError('uncertain MMIO write')
    def sync_for_device(self):self.events.append(('device',))
    def sync_for_cpu(self):self.events.append(('cpu',))
    def clock(self):return self.t
    def sleep(self,t):self.t+=max(t,.01)
    def reset_and_drain(self):
        if self.drained:self.started=False
        return self.drained
def job():return TileJob(16,16384,16384,7,248,2,32,3,2,30000,48000,0,10,1)
def executor(bus):return TileExecutor(bus,bus.clock,bus.sleep)

def test_actual_map_and_sequence():
    b=Bus();r=executor(b);r.run(job());r.run(job())
    assert b.events[0]==('r',0)
    assert ('w',0x14,0x12) in b.events and ('w',0x10,0x34567800) in b.events
    assert ('w',0x44,248) in b.events and ('w',0x74,48000) in b.events
    assert b.events.index(('device',))<b.events.index(('w',0,1))<b.events.index(('r',0x94))<b.events.index(('cpu',))
    assert not r.quarantined
@pytest.mark.parametrize('ctrl',[0,1,0x84,0x85])
def test_busy_refused_before_writes(ctrl):
    b=Bus();b.initial=ctrl
    with pytest.raises(RuntimeError):executor(b).run(job())
    assert all(e[0]=='r' for e in b.events)
def test_timeout_late_done_cannot_release_quarantine():
    b=Bus();b.done=False;r=executor(b)
    with pytest.raises(TimeoutError):r.run(job(),timeout_s=.025)
    b.done=True
    with pytest.raises(RuntimeError):r.run(job())
    with pytest.raises(RuntimeError):r.recover()
    assert r.quarantined and ('cpu',) not in b.events
    b.drained=True;r.recover();r.run(job());assert not r.quarantined
def test_uncertain_start_and_rejected_job():
    b=Bus();b.fail_start=True;r=executor(b)
    with pytest.raises(OSError):r.run(job())
    assert r.quarantined
    b=Bus();b.status=5;r=executor(b)
    with pytest.raises(TileRejected) as e:r.run(job())
    assert e.value.status==5 and not r.quarantined
@pytest.mark.parametrize('address',[-4,3,2**64-4,2**64])
def test_address_validation(address):
    b=Bus();b.dma_address=address
    with pytest.raises(ValueError):executor(b).run(job())
    assert not b.events
@pytest.mark.parametrize('timeout',[0,-1,float('nan'),float('inf')])
def test_timeout_validation(timeout):
    b=Bus()
    with pytest.raises(ValueError):executor(b).run(job(),timeout_s=timeout)
    assert not b.events
