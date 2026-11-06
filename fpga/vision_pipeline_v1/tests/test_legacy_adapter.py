import importlib.util,sys,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from legacy_adapter import detect_legacy

@unittest.skipUnless(importlib.util.find_spec('cv2') and importlib.util.find_spec('scipy'),
                     'optional legacy dependencies unavailable')
class LegacyAdapterTests(unittest.TestCase):
    def test_constant_and_empty_region(self):
        image=np.full((64,64),20,np.uint8)
        self.assertEqual(detect_legacy(image),[])
        image[32,32]=255
        self.assertEqual(detect_legacy(image,np.zeros(image.shape,bool)),[])
    def test_both_polarities_and_no_old_json_dependency(self):
        image=np.full((96,96),128,np.uint8)
        image[24:27,24:27]=220; image[68:71,68:71]=30
        results=detect_legacy(image)
        self.assertTrue(any(r['polarity']==1 for r in results))
        self.assertTrue(any(r['polarity']==-1 for r in results))
        for r in results:
            x0,y0,x1,y1=r['bbox']; self.assertTrue(0<=x0<x1<=96 and 0<=y0<y1<=96)

if __name__=='__main__': unittest.main()
