"""Optional preserved DoG proposal engine (NumPy + OpenCV + SciPy).

The original bright-field geometry()/stripe correction and old JSON merging
are deliberately not called. Supply a calibrated valid-region mask yourself.
Thresholds still need evaluation on new illumination; output is proposals only.
"""
import importlib.util
import math
from pathlib import Path
import numpy as np


def detect_legacy(image, valid=None, min_z=3.8):
    if not isinstance(image,np.ndarray) or image.dtype != np.uint8 or image.ndim!=2 or not all(image.shape):
        raise ValueError('nonempty Mono8 image required')
    if valid is None:
        valid=np.ones(image.shape,bool)
    if not isinstance(valid,np.ndarray) or valid.dtype!=np.bool_ or valid.shape!=image.shape:
        raise ValueError('same-shaped boolean valid mask required')
    if not math.isfinite(min_z) or min_z<=0:
        raise ValueError('min_z must be positive and finite')
    path=Path(__file__).resolve().parents[1]/'legacy'/'weak_detector_20260908.py'
    spec=importlib.util.spec_from_file_location('_preserved_glass_dog',path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.consolidate(module.detect_tile(image,valid,min_z=min_z))
