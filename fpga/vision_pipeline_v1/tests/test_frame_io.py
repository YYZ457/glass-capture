import sys,tempfile,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from frame_io import read_pgm,write_pgm

class FrameIoTests(unittest.TestCase):
    def test_roundtrip_binary_whitespace(self):
        with tempfile.TemporaryDirectory() as d:
            for first in (0,10,13,32,35,255):
                p=Path(d)/f'{first}.pgm'; a=np.array([[first,10,32],[0,255,35]],np.uint8)
                write_pgm(p,a); np.testing.assert_array_equal(read_pgm(p),a)
                with self.assertRaises(FileExistsError): write_pgm(p,a)
    def test_comments_and_crlf(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'x.pgm'; p.write_bytes(b'P5\r\n#hi\n2 1\n255\r\n\x0a\x20')
            np.testing.assert_array_equal(read_pgm(p),[[10,32]])
    def test_reject_incomplete_or_wrong_format(self):
        with tempfile.TemporaryDirectory() as d:
            for data in (b'P5\n2 2\n255\nabc',b'P5\n1 1\n255\nab',b'P5\n0 1\n255\n',
                         b'P5\n1 1\n65535\na',b'P2\n1 1\n255\na',b'P5\n999999 999999\n255\na'):
                p=Path(d)/'x.pgm'; p.write_bytes(data)
                with self.assertRaises(ValueError): read_pgm(p)

if __name__=='__main__': unittest.main()
