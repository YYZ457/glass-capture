import sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from pgm_stream import PgmStream

@pytest.mark.parametrize('first',[0,10,13,32,35,255])
def test_binary_first_pixel_and_fragmentation(tmp_path,first):
    raw=bytes([first,10,13,32,35,255]);path=tmp_path/'in.pgm'
    path.write_bytes(b'P5\n#comment\n3 2\n255\n'+raw)
    with PgmStream(path) as source:
        assert (source.width,source.height)==(3,2)
        chunks=list(source.chunks(2));assert max(map(len,chunks))<=2
        assert b''.join(chunks)==raw
        with pytest.raises(RuntimeError):list(source.chunks())
@pytest.mark.parametrize('data',[b'P2\n1 1\n255\nx',b'P5\n2 2\n255\nabc',
    b'P5\n1 1\n255\nab',b'P5\n16385 1\n255\nx',b'P5\n1 1\n65535\nx'])
def test_bad_frame_rejected(tmp_path,data):
    path=tmp_path/'bad.pgm';path.write_bytes(data)
    with pytest.raises(ValueError):PgmStream(path)
def test_crlf_delimiter(tmp_path):
    path=tmp_path/'in.pgm';path.write_bytes(b'P5\r\n2 1\r\n255\r\n\n ')
    with PgmStream(path) as source:assert b''.join(source.chunks())==b'\n '
