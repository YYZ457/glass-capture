import json,sys,unittest
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'python'))
from quant_runtime import REGISTERS,STATUS

class ContractTests(unittest.TestCase):
    def test_frozen_map_matches_host(self):
        contract=json.loads((root/'contract.json').read_text(encoding='utf-8'))
        offsets=contract['control']['register_byte_offsets']
        for name,value in REGISTERS.items(): self.assertEqual(offsets[name],value)
        self.assertEqual(offsets['return_status'],STATUS)
        self.assertEqual(contract['control']['protocol'],'ap_ctrl_hs')

if __name__=='__main__': unittest.main()
