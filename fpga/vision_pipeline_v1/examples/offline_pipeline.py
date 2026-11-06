"""Run from any directory; --output-dir must be new (protect captured data).

Example: python offline_pipeline.py --output-dir D:/.../demo --native-library D:/.../vision_quant.dll
Optional --input completed.pgm connects directly to sg3000-capture output.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from traditional import (BoxConfig, ComponentConfig, correct_dark_flat,
                         tiled_box_response,threshold_mask,connected_components,crop_roi)
from frame_io import read_pgm,write_pgm
from quant_runtime import Arena,NativeEngine,Job,CONV,REQUANT,MAXPOOL


def operator_demo(roi,library):
    # Demonstrates shape/layout/word offsets and exact CPU/HLS shared source.
    # Identity-like weights below are NOT a trained glass classifier.
    x=(roi.astype(np.int16)-128).astype(np.int8)[None,:,:]
    a=Arena(8192); engine=NativeEngine(library)
    inp=a.put(x); weights=a.put(np.array([2],np.int8)); acc=a.allocate(1024)
    quant=a.allocate(1024); pooled=a.allocate(256)
    multipliers=a.put(np.array([1],np.uint32)); shifts=a.put(np.array([1],np.uint32))
    engine.run(a,Job(CONV,inp,acc,1,32,32,weights_offset=weights,channels_out=1,kernel=1,stride=1))
    engine.run(a,Job(REQUANT,acc,quant,1,32,32,multiplier_offset=multipliers,shift_offset=shifts))
    np.testing.assert_array_equal(a.get(quant,(1,32,32),'int8'),x)
    engine.run(a,Job(MAXPOOL,quant,pooled,1,32,32))
    result=a.get(pooled,(1,16,16),'int8')
    np.testing.assert_array_equal(result,x.reshape(1,16,2,16,2).max(axis=(2,4)))
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path)
    p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--native-library',type=Path)
    p.add_argument('--polarity',choices=['both','bright','dark'],default='both')
    p.add_argument('--threshold',type=int,default=20)
    p.add_argument('--radius',type=int,default=4)
    args=p.parse_args()
    if args.input:
        image=read_pgm(args.input)
    else:
        image=np.full((128,192),20,np.uint8)
        image[30:34,63:67]=180 # crosses a tile edge
        image[85:89,140:144]=0
    corrected=correct_dark_flat(image)
    response=tiled_box_response(corrected.image,BoxConfig(args.radius,args.polarity),
                                corrected.valid,core_shape=(64,64))
    mask=threshold_mask(response,args.threshold)
    candidates=connected_components(mask,response.signed,ComponentConfig(min_area=2))
    args.output_dir.mkdir(parents=True,exist_ok=False)
    write_pgm(args.output_dir/'input.pgm',image)
    write_pgm(args.output_dir/'response.pgm',response.response)
    report={'schema':'vision-candidates-v1','frame_shape':list(image.shape),
            'coordinate_convention':'xyxy_half_open','source':'capture_pgm' if args.input else 'synthetic',
            'candidate_count':len(candidates),'candidates':[asdict(c) for c in candidates],
            'is_trained_model':False,'decision':'proposals_only',
            'parameters':{'polarity':args.polarity,'threshold_dn':args.threshold,'box_radius':args.radius}}
    if args.native_library:
        bbox=candidates[0].bbox if candidates else (0,0,image.shape[1],image.shape[0])
        roi=crop_roi(corrected.image,bbox,(32,32))
        result=operator_demo(roi,args.native_library)
        np.save(args.output_dir/'operator_demo_int8.npy',result)
        report['operator_demo']={'sequence':['conv1x1','requant','maxpool2x2'],
                                 'output_shape':list(result.shape),'reference_match':True}
    (args.output_dir/'candidates.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'PASS offline_pipeline: {len(candidates)} proposals; outputs: {args.output_dir}')

if __name__=='__main__': main()
