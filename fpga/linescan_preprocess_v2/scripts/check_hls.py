import argparse,hashlib,json,re,sys
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'python'))
from tile_runtime import REGISTERS,RETURN_STATUS
p=argparse.ArgumentParser(); p.add_argument('--project',type=Path,required=True)
p.add_argument('--mode',choices=['synth','cosim'],required=True); args=p.parse_args()
solution=args.project/'solution1'
headers=list(solution.glob('impl/misc/drivers/*/src/*_hw.h'))
if len(headers)!=1: raise ValueError('Expected exactly one generated register header')
text=headers[0].read_text()
expected={'AP_CTRL':0,'MEMORY_DATA':0x10,'AP_RETURN':RETURN_STATUS,
          **{k.upper()+'_DATA':v for k,v in REGISTERS.items()}}
for name,value in expected.items():
    m=re.search(r'#define\s+\w+_ADDR_'+name+r'\s+(0x[0-9a-fA-F]+)',text)
    if not m or int(m[1],16)!=value: raise ValueError('Register mismatch: '+name)
for name,bits in [('MEMORY',64)]+[(n.upper(),32) for n in REGISTERS]:
    m=re.search(r'#define\s+\w+_BITS_'+name+r'_DATA\s+(\d+)',text)
    if not m or int(m[1])!=bits: raise ValueError('Register width mismatch: '+name)
top=(solution/'syn/verilog/preprocess_tile.v').read_text()
for kind,width in [('ADDR',64),('DATA',32)]:
    if not re.search(r'C_M_AXI_DDR_'+kind+r'_WIDTH\s*=\s*'+str(width),top,re.I):
        raise ValueError('AXI '+kind+' width mismatch')
tree=ET.parse(solution/'syn/report/preprocess_tile_csynth.xml').getroot()
pipelines={}
for path in (solution/'syn/report').glob('*_csynth.xml'):
    if 'Pipeline' not in path.name: continue
    node=ET.parse(path).getroot()
    pipelines[path.stem]={e.tag:e.text for e in node.findall('.//SummaryOfOverallLatency/*')}
    pipelines[path.stem]['loops']={loop.tag:{'ii':loop.findtext('PipelineII'),'pipeline_depth':loop.findtext('PipelineDepth')}
                                  for loop in node.findall('.//SummaryOfLoopLatency/*')}
result={'register_map_verified':True,'axi_address_bits':64,'axi_data_bits':32,
        'estimated_clock_ns':tree.findtext('.//EstimatedClockPeriod'),
        'resources':{e.tag:e.text for e in tree.findall('.//AreaEstimates/Resources/*')},
        'pipelines':pipelines,'mode':args.mode,'board_tested':False,
        'source_sha256':{n:hashlib.sha256((root/'hls'/n).read_bytes()).hexdigest()
                         for n in ('preprocess_tile.cpp','preprocess_tile.h')}}
if args.mode=='cosim':
    report=(solution/'sim/report/preprocess_tile_cosim.rpt').read_text()
    if not re.search(r'\|\s*Verilog\s*\|\s*Pass\s*\|',report,re.I): raise ValueError('RTL cosim did not pass')
    result['rtl_cosim_passed']=True
(args.project/'checked-interface.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
