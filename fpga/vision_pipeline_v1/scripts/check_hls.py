"""Check real generated control map; do not trust only a tool exit code."""
import argparse,hashlib,json,re,sys
from pathlib import Path
import xml.etree.ElementTree as ET
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from quant_runtime import REGISTERS

p=argparse.ArgumentParser(); p.add_argument('--project',required=True,type=Path)
p.add_argument('--mode',choices=['synth','cosim'],required=True); args=p.parse_args()
solution=args.project/'solution1'
headers=list(solution.glob('impl/misc/drivers/*/src/*_hw.h'))
if len(headers)!=1: raise SystemExit('Expected one generated register header')
text=headers[0].read_text()
expected={'AP_CTRL':0,'MEMORY_DATA':0x10,'AP_RETURN':0xa4}
expected.update({name.upper()+'_DATA':offset for name,offset in REGISTERS.items()})
for name,offset in expected.items():
    match=re.search(r'#define\s+\w+_ADDR_'+name+r'\s+(0x[0-9a-fA-F]+)',text)
    if not match or int(match.group(1),16)!=offset:
        raise SystemExit(f'Generated register mismatch: {name}, expected {offset:#x}')
for name,bits in [('MEMORY',64)]+[(name.upper(),32) for name in REGISTERS]:
    match=re.search(r'#define\s+\w+_BITS_'+name+r'_DATA\s+(\d+)',text)
    if not match or int(match.group(1))!=bits: raise SystemExit(f'Generated width mismatch: {name}')
top=(solution/'syn/verilog/quant_engine.v').read_text()
if not re.search(r'C_M_AXI_GMEM_ADDR_WIDTH\s*=\s*64',top,re.I):
    raise SystemExit('Expected 64-bit AXI memory addresses')
if not re.search(r'C_M_AXI_GMEM_DATA_WIDTH\s*=\s*32',top,re.I):
    raise SystemExit('Expected 32-bit AXI memory data')
xml=ET.parse(solution/'syn/report/quant_engine_csynth.xml').getroot()
result={'generated_register_map_matches':True,'axi_address_bits':64,'axi_data_bits':32,
        'estimated_clock_ns':xml.findtext('.//EstimatedClockPeriod'),
        'resources':{c.tag:c.text for c in xml.findall('.//AreaEstimates/Resources/*')},
        'mode':args.mode,'board_tested':False}
source_root=Path(__file__).resolve().parents[1]
result['source_sha256']={name:hashlib.sha256((source_root/'hls'/name).read_bytes()).hexdigest()
                         for name in ('quant_engine.cpp','quant_engine.h')}
if args.mode=='cosim':
    report=solution/'sim/report/quant_engine_cosim.rpt'
    content=report.read_text()
    if not re.search(r'\|\s*Verilog\s*\|\s*Pass\s*\|',content,re.I):
        raise SystemExit('No Verilog co-simulation Pass result')
    result['rtl_cosim_passed']=True
(args.project/'checked-interface.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
