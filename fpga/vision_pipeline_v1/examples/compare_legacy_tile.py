"""Read a bounded region of an old image and exercise both proposal paths.

Pillow/OpenCV/SciPy are optional dependencies for this example. No accuracy
comparison: there are no reviewed ground-truth labels in this program.
"""
import argparse,json,sys
from dataclasses import asdict
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from traditional import box_response,BoxConfig,threshold_mask,connected_components
from legacy_adapter import detect_legacy
from frame_io import write_pgm

p=argparse.ArgumentParser(); p.add_argument('--image',required=True,type=Path)
p.add_argument('--bbox',nargs=4,type=int,required=True); p.add_argument('--output-dir',required=True,type=Path)
args=p.parse_args(); x0,y0,x1,y1=args.bbox
if not(0<=x0<x1 and 0<=y0<y1 and (x1-x0)*(y1-y0)<=1024*1024):
    raise ValueError('choose a positive tile of at most 1 megapixel')
Image.MAX_IMAGE_PIXELS=1<<28
with Image.open(args.image) as source:
    if x1>source.width or y1>source.height or source.mode!='L':
        raise ValueError('in-bounds bbox of a Mono8 image required')
    tile=np.asarray(source.crop((x0,y0,x1,y1))).copy()
response=box_response(tile,BoxConfig(4,'both'))
simple=connected_components(threshold_mask(response,3),response.signed)
legacy=detect_legacy(tile)
args.output_dir.mkdir(parents=True,exist_ok=False)
write_pgm(args.output_dir/'tile.pgm',tile)
result={'source':str(args.image),'source_bbox_xyxy':args.bbox,
        'coordinate_space':'tile local xyxy half-open','box_candidates':[asdict(c) for c in simple],
        'legacy_candidates':legacy,'box_candidate_limit':1024,'box_limit_reached':len(simple)==1024,
        'scope':'execution smoke only; not accuracy or dark-field validation'}
(args.output_dir/'proposals.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(f'PASS real-image tile: box={len(simple)}, legacy={len(legacy)}; {args.output_dir}')
