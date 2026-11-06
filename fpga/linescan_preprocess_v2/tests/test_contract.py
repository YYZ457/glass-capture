import json, sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'python'))
from tile_runtime import REGISTERS, RETURN_STATUS

def test_contract_matches_driver_map():
    c=json.loads((ROOT/'contract.json').read_text(encoding='utf-8'))
    actual=c['register_offsets_bytes']
    assert {name:actual[name] for name in REGISTERS}==REGISTERS
    assert actual['return_status']==RETURN_STATUS
    assert actual['memory_low']==0x10 and actual['memory_high']==0x14
    assert c['host_cache']['maximum_retained_input_payload_bytes_at_16k_r7']==16384*(64+14)
