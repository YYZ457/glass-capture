"""Streaming native demonstration; synthetic 16K lines or an existing P5 file.

No board/device access. Native DLL executes the same C++ as the exported IP.
Write images and JSONL incrementally; publish names only after complete input.
"""
import argparse,hashlib,json,sys,time
from dataclasses import asdict
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from pgm_stream import PgmStream
from stream_pipeline import process_chunks
from stream_components import StreamComponents
from tile_runtime import NativeEngine
from reference import decode_words

def synthetic_rows(width,height):
    for y in range(height):
        row=np.full(width,100,np.uint8)
        if 59<=y<70:row[252:min(261,width)]=220
        if 125<=y<134:row[510:min(517,width)]=10
        if y<3:row[max(0,width-4):]=230
        yield row.tobytes()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path);p.add_argument('--calibration',type=Path)
    p.add_argument('--native-library',required=True,type=Path)
    p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--radius',type=int,default=7);p.add_argument('--threshold',type=int,default=25)
    p.add_argument('--polarity',choices=['both','bright','dark'],default='both')
    args=p.parse_args();source=None
    if args.input:
        source=PgmStream(args.input);width,height=source.width,source.height;chunks=source.chunks()
    else:
        width,height=16384,135;chunks=synthetic_rows(width,height)
    calibration=np.load(args.calibration,allow_pickle=False) if args.calibration else None
    engine=NativeEngine(args.native_library);cc=StreamComponents(width,frame_id=0,min_area=2)
    args.output_dir.mkdir(parents=True,exist_ok=False)
    state={'status':'running','source':str(args.input) if args.input else 'synthetic',
           'width':width,'height':height,'radius':args.radius,'threshold':args.threshold,'polarity':args.polarity,
           'implementation':'native C++ shared with FPGA IP','board_tested':False,'accuracy_evaluated':False}
    state_path=args.output_dir/'run.json';state_path.write_text(json.dumps(state,indent=2),encoding='utf-8')
    started=time.perf_counter();digest=hashlib.sha256();count=0;highwater=0;stripes=0
    try:
        with (args.output_dir/'mask.pgm.partial').open('xb') as mask_file, \
             (args.output_dir/'corrected.pgm.partial').open('xb') as corrected_file, \
             (args.output_dir/'components.jsonl.partial').open('x',encoding='utf-8') as candidates:
            header=f'P5\n{width} {height}\n255\n'.encode('ascii')
            mask_file.write(header);corrected_file.write(header)
            def save(items):
                for component in items:candidates.write(json.dumps(asdict(component),ensure_ascii=False)+'\n')
                return len(items)
            for stripe in process_chunks(chunks,width,height,engine,radius=args.radius,
                       calibration=calibration,polarity=['both','bright','dark'].index(args.polarity),threshold=args.threshold):
                decoded=decode_words(stripe.words)
                digest.update(stripe.words.astype('<u4',copy=False).tobytes())
                mask_file.write((decoded['mask'].astype(np.uint8)*255).tobytes())
                corrected_file.write(decoded['corrected'].tobytes())
                for signed,mask in zip(decoded['signed'],decoded['mask']):count+=save(cc.consume_row(signed,mask))
                highwater=max(highwater,stripe.input_cache_high_water_bytes);stripes+=1
            count+=save(cc.finish())
        for name in ('mask.pgm','corrected.pgm','components.jsonl'):
            (args.output_dir/(name+'.partial')).rename(args.output_dir/name)
        state.update(status='complete',components=count,stripes=stripes,retained_input_cache_peak_bytes=highwater,
                     output_words_sha256=digest.hexdigest(),native_wall_seconds=time.perf_counter()-started)
    except BaseException as error:
        state.update(status='failed',error=str(error));raise
    finally:
        if source is not None:source.close()
        state_path.write_text(json.dumps(state,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(state,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
