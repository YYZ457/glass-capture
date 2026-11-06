import os,sys
from pathlib import Path
import numpy as np
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'python'))
from tile_runtime import NativeEngine,TileJob,TileRejected,REGISTERS
from reference import reference_words,decode_words,make_column_calibration
from stream_pipeline import process_chunks
from stream_components import StreamComponents
from strip_cache import StripCache,iter_tiles,Stripe
from tile_runtime import StripeArena
from dataclasses import replace


@pytest.fixture(scope='module')
def engine():
    path=os.environ.get('LINESCAN_NATIVE_LIBRARY')
    if not path: pytest.fail('Run verify.ps1: LINESCAN_NATIVE_LIBRARY is required, not silently skipped')
    return NativeEngine(path)


def fragments(data):
    offset=0;sizes=[1,3,65535,4097,7]
    i=0
    while offset<len(data):
        n=sizes[i%len(sizes)];yield memoryview(data)[offset:offset+n];offset+=n;i+=1


@pytest.mark.parametrize('width,height,radius,ch,cw',[(1,1,0,1,1),(3,2,7,1,2),
    (263,19,4,7,127),(16384,135,7,64,256),(16381,9,2,4,251)])
def test_native_stream_equal_to_whole_image(engine,width,height,radius,ch,cw):
    image=np.full((height,width),70,np.uint8)
    image[max(0,height//2-2):min(height,height//2+3),max(0,width//2-2):min(width,width//2+3)]=220
    if width>260 and height>65:
        image[61:68,253:260]=10
        image[0:4,-4:]=230;image[-4:,0:4]=0
    # Column gain is Q8.16, invalid columns must erode neighborhood validity.
    calibration=np.full(width,((81920<<8)|5),np.uint32)
    if width>777:calibration[777]=0
    expected=reference_words(image,radius,calibration,0,12)
    records=list(process_chunks(fragments(image.tobytes()),width,height,engine,radius=radius,
                                core_rows=ch,core_width=cw,calibration=calibration,threshold=12,frame_id=37))
    actual=np.concatenate([r.words for r in records])
    np.testing.assert_array_equal(actual,expected)
    assert [r.y0 for r in records]==list(range(0,height,ch))
    assert all(r.frame_id==37 and not r.words.flags.writeable for r in records)
    assert max(r.input_cache_high_water_bytes for r in records)<=width*(ch+2*radius)
    if width==16384:assert max(r.input_cache_high_water_bytes for r in records)==16384*78


def flood(mask,signed):
    seen=set();found=[];h,w=mask.shape
    ys,xs=np.nonzero(mask)
    for y,x in zip(ys.tolist(),xs.tolist()):
        if (y,x) in seen:continue
        stack=[(y,x)];seen.add((y,x));positive=signed[y,x]>0;pixels=[]
        while stack:
            cy,cx=stack.pop();pixels.append((cy,cx))
            for ny in range(max(0,cy-1),min(h,cy+2)):
                for nx in range(max(0,cx-1),min(w,cx+2)):
                    if mask[ny,nx] and (ny,nx) not in seen and (signed[ny,nx]>0)==positive:
                        seen.add((ny,nx));stack.append((ny,nx))
        py,px=zip(*pixels)
        found.append(((min(px),min(py),max(px)+1,max(py)+1),'bright' if positive else 'dark',
                      len(pixels),max(abs(int(signed[cy,cx])) for cy,cx in pixels)))
    return sorted(found)


def test_no_cross_tile_or_stripe_component_break(engine):
    image=np.full((135,16384),100,np.uint8)
    image[61:69,253:261]=220
    image[126:134,510:516]=10
    image[0:3,16380:]=230
    stream=StreamComponents(16384,frame_id=7)
    components=[]
    for rec in process_chunks(fragments(image.tobytes()),16384,135,engine,radius=3,threshold=35,frame_id=7):
        decoded=decode_words(rec.words)
        for s,m in zip(decoded['signed'],decoded['mask']): components.extend(stream.consume_row(s,m))
        assert stream.active_state_count<=32768
    components.extend(stream.finish())
    full=decode_words(reference_words(image,3,threshold=35))
    assert sorted((c.bbox,c.polarity,c.area,c.peak) for c in components)==flood(full['mask'],full['signed'])
    assert all(c.frame_id==7 for c in components)
    assert any(c.bbox[0]<256<c.bbox[2] and c.bbox[1]<64<c.bbox[3] for c in components)


def test_snapshot_stays_valid_and_truncated_frame_rejected(engine):
    image=np.arange(16*12,dtype=np.uint8).reshape(12,16)
    generator=process_chunks([image.tobytes()],16,12,engine,radius=1,core_rows=4)
    first=next(generator);saved=first.words.copy()
    list(generator);np.testing.assert_array_equal(first.words,saved)
    with pytest.raises(ValueError):list(process_chunks([image.tobytes()[:-1]],16,12,engine))


def test_calibration_numeric_limits():
    dark=np.array([0,10,100,255],np.uint8);flat=np.array([255,11,100,0],np.uint8)
    words=make_column_calibration(dark,flat)
    assert words[0]==65536<<8 and words[1]==((255*65536)<<8)|10
    assert words[2]==0 and words[3]==0
    result=decode_words(reference_words(np.array([[255,11,100,255]],np.uint8),0,words))
    np.testing.assert_array_equal(result['corrected'],[[255,255,0,0]])
    np.testing.assert_array_equal(result['valid'],[[True,True,False,False]])


def test_native_rejection_preserves_arena(engine):
    memory=np.full(512,0xa5a5a5a5,np.uint32);before=memory.copy()
    job=TileJob(0,16,16,8,0,0,16,8,3,0,0)
    with pytest.raises(TileRejected) as exc:engine.run(memory,job)
    assert exc.value.status==5
    np.testing.assert_array_equal(before,memory)
    assert REGISTERS['flags']==0x8c


def test_missing_halo_or_wrong_core_rejected_before_execution():
    cache=StripCache(20,20,core_rows=8,radius=1,frame_id=3)
    cache.push(bytes(400));stripe=cache.pop();tile=next(iter_tiles(stripe))
    arena=StripeArena(stripe)
    with pytest.raises(ValueError):arena.job(tile,2)
    with pytest.raises(ValueError):arena.job(replace(tile,core_y=tile.core_y+1),1)
    with pytest.raises(ValueError):arena.job(replace(tile,frame_id=4),1)
    without_metadata=Stripe(stripe.data,20,stripe.source_y,stripe.core_y,stripe.core_rows,3,stripe.core_start)
    with pytest.raises(ValueError):StripeArena(without_metadata)


def test_calibration_generation_frozen_for_whole_frame(engine):
    image=np.arange(16*8,dtype=np.uint8).reshape(8,16)
    cal=np.full(16,65536<<8,np.uint32);before=cal.copy()
    stream=process_chunks([image.tobytes()],16,8,engine,radius=1,core_rows=4,calibration=cal)
    first=next(stream);cal[:]=0
    words=np.concatenate([first.words]+[r.words for r in stream])
    np.testing.assert_array_equal(words,reference_words(image,1,before))


def test_invalid_output_encoding_refused():
    for value in (1<<31,(256<<16)|(1<<25),1<<16,1<<26):
        with pytest.raises(ValueError):decode_words(np.array([value],np.uint32))
