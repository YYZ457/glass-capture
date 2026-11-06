import copy,json,sys,time
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'console'))
from transport import Console
from telemetry import parse

def records():
    return [json.loads(line[7:]) for line in (ROOT/'evidence/synthetic-stream.jsonl').read_text().splitlines()]
def line(item):return 'SGJSON '+json.dumps(item)

@pytest.mark.parametrize('field,value',[
 ('sum','99999999999999999999'),('sequence','-1'),('schema',True),('width',True),
 ('pixel_count','1'),('above_threshold','99999999'),('min',256),('decision','pass'),
 ('received_ns','18446744073709551616'),('processed_total','2'),('flags',4),('source','camera')])
def test_invalid_measurements_rejected(field,value):
    data=records()[1];data[field]=value
    assert parse(line(data),'recorded-replay') is None

def test_incomplete_ended_never_confirms_success(tmp_path):
    c=Console(tmp_path)
    c.accept_stream_line('SGJSON {"schema":1,"event":"ended","error":0,"stop_error":0}','board')
    assert not c.stream

def test_ended_stop_failure_is_error_and_terminal(tmp_path):
    c=Console(tmp_path);data=records()[-1];data['stop_error']=-5
    c.accept_stream_line(line(data),'recorded-replay')
    assert c.stream['status']=='error'
    c.accept_stream_line(line(records()[0]),'recorded-replay')
    assert c.stream['status']=='error'

def test_result_needs_start_and_monotonic_sequence(tmp_path):
    c=Console(tmp_path);r=records()
    c.accept_stream_line(line(r[1]),'recorded-replay');assert not c.stream
    c.accept_stream_line(line(r[0]),'recorded-replay')
    c.accept_stream_line(line(r[2]),'recorded-replay')
    c.accept_stream_line(line(r[1]),'recorded-replay')
    assert c.stream['result']['sequence']=='2'
    snapshot=c.state();snapshot['stream']['result']['sequence']='corrupted'
    assert c.stream['result']['sequence']=='2'

def test_board_channel_rejects_synthetic(tmp_path):
    c=Console(tmp_path)
    c.accept_stream_line(line(records()[0]),'board')
    assert not c.stream


def test_orphan_success_and_inconsistent_totals_rejected(tmp_path):
    c=Console(tmp_path);r=records()
    c.accept_stream_line(line(r[-1]),'recorded-replay');assert not c.stream
    c.accept_stream_line(line(r[0]),'recorded-replay')
    c.accept_stream_line(line(r[2]),'recorded-replay')
    end=r[-1];end['received']='1';end['processed']='1'
    c.accept_stream_line(line(end),'recorded-replay');assert c.stream['status']=='running'
    end=records()[-1];end['queue_peak']=r[0]['queue_depth']+1
    c.accept_stream_line(line(end),'recorded-replay');assert c.stream['status']=='running'

class PartialWriter:
    def __init__(self,count=3):self.data=bytearray();self.count=count
    def write(self,data):
        n=min(self.count,len(data));self.data.extend(data[:n]);return n

def test_partial_serial_write_preserves_all_bytes(tmp_path):
    c=Console(tmp_path);c.connected=True;c.port=PartialWriter()
    payload=b'abc123'*40;c._write(payload)
    assert c.port.data==payload

def test_zero_write_clears_identity_and_unknown_stream(tmp_path):
    c=Console(tmp_path);c.connected=True;c.port=PartialWriter(0)
    c.shell_ready=c.identity_ok=True;c.stream={'channel':'board','status':'running'}
    with pytest.raises(ConnectionError):c.exchange('echo test')
    assert not c.shell_ready and not c.identity_ok and c.stream['status']=='unknown'

def test_stream_exception_does_not_stay_running(tmp_path):
    c=Console(tmp_path);c.stream={'channel':'board','status':'running'}
    c.job={'action':'stream','status':'running'};c.busy=True
    def failed():raise ConnectionError('connection lost')
    c._stream=failed;c._job('stream')
    assert c.stream['status']=='unknown' and c.job['status']=='error' and not c.busy

def test_log_names_unique(tmp_path):
    assert Console(tmp_path).log_path!=Console(tmp_path).log_path
