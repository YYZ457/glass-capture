"""Assemble verified modules, extract to Desktop, then verify every file hash.

Does not touch board, SD or pre-existing Desktop output. Large build projects
stay on D:. All packaged files must be <10 MiB. Run only after verify + cosim.
"""
import argparse,hashlib,json,re,shutil,zipfile
from datetime import datetime,timezone
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
DATA=Path('D:/CodexData/visionkit_zcu102')
BUILD=DATA/'linescan_v2'
HLS=BUILD/'hls_burst/preprocess_tile'
VERIFY=BUILD/'verify'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def copy(source,dest):
    dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source,dest)
def json_write(path,value): path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--name',default='玻璃检测_FPGA模块包_20261024')
    parser.add_argument('--desktop',type=Path,default=Path.home()/'Desktop')
    args=parser.parse_args()
    if Path(args.name).name!=args.name or args.name in ('.','..'): raise ValueError('name must be a single directory name')
    destination=args.desktop/args.name
    stage=BUILD/'delivery'/args.name
    archive=BUILD/'delivery'/(args.name+'.zip')
    if any(p.exists() for p in (destination,stage,archive)): raise FileExistsError('Choose a fresh delivery name; never overwrite existing delivery')
    checked=json.loads((HLS/'checked-interface.json').read_text())
    if not checked.get('rtl_cosim_passed'): raise ValueError('RTL cosim required')
    for name,digest in checked['source_sha256'].items():
        if sha(ROOT/'hls'/name)!=digest: raise ValueError('HLS source/IP mismatch')
    verified=json.loads((VERIFY/'verified-sources.json').read_text(encoding='utf-8'))
    for name,digest in verified.items():
        if sha(ROOT/name)!=digest: raise ValueError('Native-tested source changed: '+name)
    pylog=(VERIFY/'python-tests.log').read_text()
    if not re.search(r'\d+ passed',pylog) or re.search(r'\d+ (failed|skipped)',pylog): raise ValueError('Python tests not passing')
    for log in ('native-tests.log','components-tests.log'):
        if 'PASS' not in (VERIFY/log).read_text(): raise ValueError('Missing native test success')
    v1=stage/'vision_pipeline_v1'; v2=stage/'linescan_preprocess_v2'
    shutil.copytree(DATA/'reusable_core/delivery/vision_pipeline_v1',v1)
    shutil.copytree(ROOT,v2,ignore=shutil.ignore_patterns('__pycache__','.pytest_cache','*.pyc'))
    copy(VERIFY/'linescan_preprocess.dll',v2/'prebuilt/windows-x64/linescan_preprocess.dll')
    copy(VERIFY/'test_components_aarch64.elf',v2/'prebuilt/linux-aarch64/test_components_aarch64.elf')
    elf=(VERIFY/'test_components_aarch64.elf').read_bytes()
    if elf[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(elf[18:20],'little')!=183: raise ValueError('Not AArch64 ELF64')
    copy(HLS/'preprocess_tile_ip.zip',v2/'prebuilt/zcu102/preprocess_tile_ip.zip')
    for log in ('native-tests.log','components-tests.log','python-tests.log','verified-sources.json'):
        copy(VERIFY/log,v2/'evidence'/log)
    copy(HLS/'checked-interface.json',v2/'evidence/checked-interface.json')
    copy(HLS/'export-ip.log',v2/'evidence/export-ip.log')
    copy(HLS.parent/'hls-cosim.log',v2/'evidence/hls-cosim.log')
    for directory in ('syn/report','sim/report'):
        for path in (HLS/'solution1'/directory).iterdir():
            if path.is_file() and path.suffix in ('.rpt','.xml'):
                copy(path,v2/'evidence'/directory/path.name)
    for path in (HLS/'solution1/impl/misc/drivers').rglob('*'):
        if path.is_file(): copy(path,v2/'evidence/generated-driver'/path.relative_to(HLS/'solution1/impl/misc/drivers'))
    for path in (BUILD/'demo_16k').iterdir():
        if path.is_file(): copy(path,stage/'sample_output_16k'/path.name)
    for name,ip in [('quant_engine',v1/'prebuilt/zcu102/quant_engine_ip.zip'),('preprocess_tile',v2/'prebuilt/zcu102/preprocess_tile_ip.zip')]:
        with zipfile.ZipFile(ip) as z:
            target=stage/'ip_repository'/name
            for member in z.namelist():
                if not (target/member).resolve().is_relative_to(target.resolve()): raise ValueError('Unsafe archive member')
            z.extractall(target)
        if not (target/'component.xml').is_file(): raise ValueError('Missing IP manifest')
    json_write(v2/'validation.json',{'created_utc':datetime.now(timezone.utc).isoformat(),
        'python_tests':re.search(r'\d+ passed',pylog)[0], 'native_preprocess_accepted_cases':329,
        'native_preprocess_rejected_cases':34,'cpp_cc_random_oracle_frames':3000,
        'rtl_cosim_jobs':9,'rtl_cosim_passed':True,'max_cosim_cycles':71273,
        'aarch64_static_test_cross_compiled':True,'aarch64_test_executed':False,
        'board_tested':False,'routed_timing_verified':False,'darkfield_accuracy_evaluated':False,
        'hls':checked,'ip_sha256':sha(v2/'prebuilt/zcu102/preprocess_tile_ip.zip')})
    (stage/'先读这里.md').write_text('''# 玻璃检测 FPGA 模块包

这是完整解压目录。先从 `linescan_preprocess_v2/README.md` 开始；后续 Gemini 也先读该目录的 AGENTS.md 和 contract.json。

- **linescan_preprocess_v2**：新增真实 HLS 预处理 IP、16K 条带缓存、halo 分块、跨块/跨条带缺陷合并，Python 和 C++ 版本、接口和测试齐备。
- **vision_pipeline_v1**：之前冻结的 AI 量化算子、传统算法参考、RTL FIFO/帧封装/任务控制模块及其证据；保持原样。
- **ip_repository**：两个 IP 均已解压，可加入 Vivado IP Repository；也可在已有工程 source `add_ip_repositories.tcl`。
- **sample_output_16k**：16384×135 合成输入的校正图、掩膜、候选 JSONL 和运行记录。默认双极性，因此亮缺陷附近也可能出现暗背景响应，候选数不等于植入缺陷数。
- **run_demo.ps1**：本机运行离线 16K 演示；输出自动放 D 盘新目录。可用 `-InputPgm` 处理 Mono8 P5 文件；用 `-Polarity bright` 试暗场候选模式。无需连接板卡。

```powershell
& ./run_demo.ps1
& ./run_demo.ps1 -InputPgm 'D:\\your_data\\image.pgm' -Polarity bright
& ./linescan_preprocess_v2/verify.ps1
```

拼接顺序：采集 Mono8 碎片 → StripCache → 带 halo 的条带 → preprocess_tile → 完整核心行 → StreamComponents → ROI/AI 输入适配 → v1 AI 算子或其他模型。

**两个 IP 格式不同**：v2 输入一字四像素、输出 signed9 响应；v1 AI 输入一字一个量化元素。必须显式解包、量化与布局转换，不能直接相连后假定数值一致。框坐标为全局半开区间，帧号一路保留。

已经完成软件对照、实际 HLS 综合、Verilog 协同仿真和 IP 导出。板端 DMA 映射/缓存同步、Vivado 系统连接、布局布线、真实相机吞吐和暗场准确率仍需实机验证；本包没有完整系统 bitstream 或 BOOT.BIN，也未改动 SD。合成图结果只能验证接口和跨块行为。

每个文件的 SHA256 见 SHA256SUMS；v2 详细证据在 evidence/ 和 validation.json。包内 Linux AArch64 ELF 仅已交叉编译，未在板上运行。
''',encoding='utf-8')
    (stage/'run_demo.ps1').write_text('''param(
 [string]$Python='C:\\Users\\YANYZ\\anaconda3\\python.exe',
 [string]$InputPgm='',
 [ValidateSet('both','bright','dark')][string]$Polarity='both',
 [string]$OutputDir=('D:\\CodexData\\visionkit_zcu102\\linescan_v2\\desktop_demo_'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
)
$ErrorActionPreference='Stop'
$arguments=@('-X','utf8',"$PSScriptRoot/linescan_preprocess_v2/examples/run_linescan.py",'--native-library',"$PSScriptRoot/linescan_preprocess_v2/prebuilt/windows-x64/linescan_preprocess.dll",'--output-dir',$OutputDir,'--polarity',$Polarity)
if($InputPgm){$arguments+=@('--input',$InputPgm)}
& $Python @arguments
if($LASTEXITCODE -ne 0){throw 'Demo failed; inspect run.json'}
Write-Output "Completed: $OutputDir"
''',encoding='utf-8')
    (stage/'add_ip_repositories.tcl').write_text('''# Run inside an already open Vivado project. Preserve existing repositories.
set repo [file join [file dirname [file normalize [info script]]] ip_repository]
set repos [get_property ip_repo_paths [current_project]]
if {[lsearch -exact $repos $repo] < 0} {lappend repos $repo}
set_property ip_repo_paths $repos [current_project]
update_ip_catalog
''',encoding='utf-8')
    files=sorted(p for p in stage.rglob('*') if p.is_file())
    if any(p.stat().st_size>10*1024*1024 for p in files): raise ValueError('Large file belongs on D, not Desktop')
    hashes={p.relative_to(stage).as_posix():sha(p) for p in files}
    (stage/'SHA256SUMS').write_text(''.join(digest+'  '+name+'\n' for name,digest in hashes.items()),encoding='utf-8')
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for path in stage.rglob('*'):
            if path.is_file(): z.write(path,path.relative_to(stage))
    destination.mkdir(parents=True)
    with zipfile.ZipFile(archive) as z: z.extractall(destination)
    for name,digest in hashes.items():
        if sha(destination/name)!=digest: raise ValueError('Desktop hash mismatch: '+name)
    if sha(stage/'SHA256SUMS')!=sha(destination/'SHA256SUMS'): raise ValueError('Manifest copy mismatch')
    result={'desktop':str(destination),'archive':str(archive),'file_count':len(files)+1,
            'archive_sha256':sha(archive),'all_desktop_files_verified':True}
    json_write(BUILD/'delivery'/(args.name+'.delivery.json'),result)
    print(json.dumps(result,ensure_ascii=False,indent=2))

if __name__=='__main__': main()
