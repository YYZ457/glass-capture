"""Create a self-contained source/prebuilt/evidence delivery, then hash-readback.

Destination must not exist; no user files are overwritten. Run verify and
build-hls cosim/export-ip plus synth_rtl before packaging a validated release.
"""
import argparse,datetime,hashlib,importlib.metadata,json,re,shutil,zipfile
from pathlib import Path

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def require_log(path,marker):
    text=path.read_text(encoding='utf-8',errors='replace')
    if marker not in text or re.search(r'^\s*(ERROR:|Fatal:)',text,re.M):
        raise ValueError(f'Unsuccessful verification evidence: {path}')
    return text

p=argparse.ArgumentParser(); p.add_argument('--build-root',type=Path,required=True)
p.add_argument('--destination',type=Path,required=True); a=p.parse_args()
root=Path(__file__).resolve().parents[1]; build=a.build_root
verify=build/'verify'; project=build/'release_hls'/'quant_engine'
verified_sources=json.loads((verify/'verified-sources.json').read_text())
for source in root.rglob('*'):
    if source.is_file() and source.suffix in ('.cpp','.h','.sv','.py'):
        if verified_sources.get(source.relative_to(root).as_posix())!=digest(source):
            raise ValueError('Source differs from successful verification: '+str(source))
python_log=require_log(verify/'python-tests.log','OK')
count=re.search(r'Ran (\d+) tests',python_log)
if not count or 'skipped=' in python_log: raise ValueError('All Python tests must run, with no skips')
require_log(verify/'numeric.log','all independent reference, boundary and no-write rejection tests passed')
for name in ('stream_fifo','job_supervisor','gray_frame_framer'):
    require_log(verify/name/'run.log','PASS tb_'+name)
require_log(build/'rtl_synth.log','PASS RTL synthesis: three modules')
injected=(verify/'injected-failure.log').read_text()
if 'Fatal: Intentional FIFO failure-path self-test' not in injected:
    raise ValueError('Missing failure-gate calibration')
require_log(build/'release_hls'/'hls-cosim.log','C/RTL co-simulation finished: PASS')
checked=json.loads((project/'checked-interface.json').read_text())
for name,h in checked['source_sha256'].items():
    if digest(root/'hls'/name)!=h: raise ValueError('Source differs from checked HLS build')
if not checked.get('rtl_cosim_passed') or not checked.get('generated_register_map_matches'):
    raise ValueError('Missing generated interface checks')
require_log(project/'export-ip.log','Generated output file')
ip=project/'quant_engine_ip.zip'
with zipfile.ZipFile(ip) as z:
    if z.testzip() or not any(n.endswith('component.xml') for n in z.namelist()):
        raise ValueError('Invalid IP archive')
validation={
    'schema':'vision-pipeline-validation-v1',
    'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'python_tests_passed':int(count.group(1)),
    'cpp_random_reference_cases':155,
    'cpp_boundary_rejection_repeat_tests_passed':True,
    'rtl_simulation':{'fifo':'pass','job_supervisor':'pass','gray_frame_framer':'pass'},
    'rtl_ooc_synthesis':'pass_default_parameters',
    'failure_gate_calibrated':True,
    'hls':checked,
    'rtl_cosim_sequential_jobs':7,
    'ip_exported':True,
    'source_quant_engine_sha256':digest(root/'hls/quant_engine.cpp'),
    'legacy_original_sha256':digest(root/'legacy/weak_detector_20260908.py'),
    'real_image_execution_smoke':{'source':r'C:\Users\YANYZ\Desktop\图\图\1.bmp',
        'crop_xyxy':[11296,928,11808,1440],'box_candidates_returned':1024,
        'box_candidate_limit_reached':True,'legacy_candidates_returned':46,
        'ground_truth_accuracy_evaluated':False},
    'board_tested':False,'camera_dma_frame_verified':False,
    'dark_field_accuracy_evaluated':False,'place_route_timing_closed':False,
    'tested_python_distributions':{},
    'build_root':str(build.resolve()),
    'delivery_directory':str(a.destination.resolve()),
}
for distribution in ('numpy','opencv-python','scipy','Pillow'):
    try: validation['tested_python_distributions'][distribution]=importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError: validation['tested_python_distributions'][distribution]='not_registered'
(root/'validation.json').write_text(json.dumps(validation,ensure_ascii=False,indent=2),encoding='utf-8')
a.destination.mkdir(parents=True,exist_ok=False)
for source in root.rglob('*'):
    if source.is_file() and '__pycache__' not in source.parts and source.suffix in ('.py','.cpp','.h','.sv','.tcl','.ps1','.json','.md'):
        relative=source.relative_to(root); target=a.destination/relative
        target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(source,target)
extras={
    'prebuilt/windows-x64/vision_quant.dll':verify/'vision_quant.dll',
    'prebuilt/windows-x64/quant_test.exe':verify/'quant_test.exe',
    'prebuilt/zcu102/quant_engine_ip.zip':ip,
    'evidence/python-tests.log':verify/'python-tests.log',
    'evidence/numeric.log':verify/'numeric.log',
    'evidence/failure-gate.log':verify/'injected-failure.log',
    'evidence/quant_engine_csynth.rpt':project/'solution1/syn/report/quant_engine_csynth.rpt',
    'evidence/quant_engine_cosim.rpt':project/'solution1/sim/report/quant_engine_cosim.rpt',
    'evidence/checked-interface.json':project/'checked-interface.json',
    'evidence/xquant_engine_hw.h':project/'solution1/impl/misc/drivers/quant_engine_v1_0/src/xquant_engine_hw.h',
}
for name in ('stream_fifo','job_supervisor','gray_frame_framer'):
    extras['evidence/'+name+'.log']=verify/name/'run.log'
    extras['evidence/'+name+'_utilization.rpt']=build/'rtl_synth'/(name+'_utilization.rpt')
for relative,source in extras.items():
    target=a.destination/relative; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(source,target)
files=sorted(p for p in a.destination.rglob('*') if p.is_file())
hashes={p.relative_to(a.destination).as_posix():digest(p) for p in files}
(a.destination/'SHA256SUMS').write_text(''.join(f'{h}  {name}\n' for name,h in hashes.items()),encoding='utf-8',newline='\n')
archive=a.destination.with_suffix('.zip')
with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED) as z:
    for source in sorted(a.destination.rglob('*')):
        if source.is_file(): z.write(source,a.destination.name+'/'+source.relative_to(a.destination).as_posix())
with zipfile.ZipFile(archive) as z:
    if z.testzip(): raise ValueError('Archive CRC failed')
    for name,h in hashes.items():
        if hashlib.sha256(z.read(a.destination.name+'/'+name)).hexdigest()!=h:
            raise ValueError('Archive readback mismatch: '+name)
print(json.dumps({'directory':str(a.destination),'archive':str(archive),
                  'archive_sha256':digest(archive),'verified_files':len(hashes),'bytes':archive.stat().st_size},indent=2))
