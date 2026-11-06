"""Independent whole-image integer oracle for the cached FPGA tile core.

This uses summed-area NumPy operations, not the hardware's rotating row banks.
The full-image allocation is for testing only, not the streaming receiver.
"""
import numpy as np


def make_column_calibration(dark, flat, target=255, min_denominator=1):
    """Compile measured uint8 column dark/flat levels to Q8.16 gain words.

    Gain uses nearest/half-up rounding. Invalid denominator becomes word zero.
    This quantized gain contract may differ by 1 DN from per-pixel division.
    Calibration must be measured separately; no online scene-based updating.
    """
    if not isinstance(dark,np.ndarray) or dark.ndim!=1 or dark.dtype!=np.uint8:
        raise ValueError('dark must be a uint8 column vector')
    if not isinstance(flat,np.ndarray) or flat.dtype!=np.uint8 or flat.shape!=dark.shape or not dark.size:
        raise ValueError('flat must match the nonempty uint8 dark vector')
    if type(target) is not int or not 1<=target<=255 or type(min_denominator) is not int or not 1<=min_denominator<=255:
        raise ValueError('target and minimum denominator must be integers in 1..255')
    den=flat.astype(np.int32)-dark.astype(np.int32)
    valid=den>=min_denominator
    safe=np.maximum(den,1).astype(np.uint64)
    gain=((target<<16)+safe//2)//safe
    gain=np.minimum(gain,0xffffff).astype(np.uint32)
    return np.where(valid,(gain<<8)|dark.astype(np.uint32),0).astype(np.uint32)


def correct_columns(image, calibration=None):
    if not isinstance(image,np.ndarray) or image.dtype!=np.uint8 or image.ndim!=2 or not all(image.shape):
        raise ValueError('nonempty Mono8 array required')
    if calibration is None:
        return image.copy(),np.ones(image.shape,dtype=bool)
    if not isinstance(calibration,np.ndarray) or calibration.dtype!=np.uint32 or calibration.shape!=(image.shape[1],):
        raise ValueError('calibration must contain one uint32 per image column')
    dark=(calibration&255).astype(np.int32)
    gain=(calibration>>8).astype(np.uint64)
    signal=np.maximum(image.astype(np.int32)-dark,0).astype(np.uint64)
    corrected=np.minimum((signal*gain+32768)>>16,255).astype(np.uint8)
    valid=np.broadcast_to(gain>0,image.shape).copy()
    corrected[~valid]=0
    return corrected,valid


def _sum_square(a,radius):
    p=np.pad(a,((radius,radius),(radius,radius)),mode='edge').astype(np.int64)
    integral=np.pad(p.cumsum(axis=0).cumsum(axis=1),((1,0),(1,0)))
    k=2*radius+1
    return integral[k:,k:]-integral[:-k,k:]-integral[k:,:-k]+integral[:-k,:-k]


def reference_words(image, radius=4, calibration=None, polarity=0, threshold=10):
    if type(radius) is not int or not 0<=radius<=7: raise ValueError('radius must be 0..7')
    if type(polarity) is not int or polarity not in (0,1,2): raise ValueError('polarity must be 0,1,2')
    if type(threshold) is not int or not 1<=threshold<=255: raise ValueError('threshold must be 1..255')
    corrected,valid=correct_columns(image,calibration)
    count=(2*radius+1)**2
    background=((_sum_square(corrected,radius)+count//2)//count).astype(np.uint32)
    neighborhood_valid=_sum_square(valid,radius)==count
    signed=np.where(neighborhood_valid,corrected.astype(np.int32)-background.astype(np.int32),0)
    amplitude=np.abs(signed) if polarity==0 else np.maximum(signed if polarity==1 else -signed,0)
    passed=neighborhood_valid&(amplitude>=threshold)
    return (corrected.astype(np.uint32)|(background<<8)|((signed&511).astype(np.uint32)<<16)|
            (neighborhood_valid.astype(np.uint32)<<25)|(passed.astype(np.uint32)<<26))


def decode_words(words):
    if not isinstance(words,np.ndarray) or words.dtype!=np.uint32:
        raise ValueError('uint32 output required')
    if np.any(words>>27): raise ValueError('reserved output bits are nonzero')
    signed=((words>>16)&511).astype(np.int16)
    signed=np.where(signed>=256,signed-512,signed).astype(np.int16)
    valid=((words>>25)&1).astype(bool); mask=((words>>26)&1).astype(bool)
    if np.any(signed==-256) or np.any((~valid)&(signed!=0)):
        raise ValueError('invalid signed-response encoding')
    if np.any(mask&(~valid | (signed==0))): raise ValueError('invalid threshold output')
    return {'corrected':(words&255).astype(np.uint8),'background':((words>>8)&255).astype(np.uint8),
            'signed':signed,'valid':valid,'mask':mask}
