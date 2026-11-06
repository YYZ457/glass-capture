"""Strict P5 Mono8 frame boundary for sg3000-capture's completed PGM files."""
from pathlib import Path
import numpy as np


def read_pgm(path, max_pixels=1 << 28):
    # Bound before reading payload; header capped at 4 KiB.
    with Path(path).open('rb') as f:
        header=f.read(4096)
        index=0
        tokens=[]
        for _ in range(4):
            while index<len(header):
                if header[index] in b' \t\r\n': index+=1
                elif header[index]==35:
                    end=header.find(b'\n',index)
                    if end<0: raise ValueError('incomplete PGM comment')
                    index=end+1
                else: break
            start=index
            while index<len(header) and header[index] not in b' \t\r\n#': index+=1
            if index==start or index==len(header): raise ValueError('invalid or oversized PGM header')
            tokens.append(header[start:index])
        if tokens[0]!=b'P5' or tokens[3]!=b'255': raise ValueError('P5 Mono8 maxval255 required')
        if any(not t.isdigit() for t in tokens[1:3]): raise ValueError('invalid dimensions')
        width,height=map(int,tokens[1:3])
        if width<=0 or height<=0 or width*height>max_pixels: raise ValueError('invalid or oversized frame')
        if header[index] not in b' \t\r\n': raise ValueError('missing binary delimiter')
        # Consume one delimiter only: binary pixels may themselves be whitespace.
        if header[index:index+2]==b'\r\n': index+=2
        else: index+=1
        f.seek(0,2)
        if f.tell()-index!=width*height: raise ValueError('truncated frame or trailing data')
        f.seek(index)
        return np.frombuffer(f.read(width*height),np.uint8).reshape(height,width).copy()


def write_pgm(path,image):
    if not isinstance(image,np.ndarray) or image.dtype!=np.uint8 or image.ndim!=2 or not all(image.shape):
        raise ValueError('nonempty Mono8 required')
    # Exclusive create: examples must never silently replace captured data.
    with Path(path).open('xb') as f:
        f.write(f'P5\n{image.shape[1]} {image.shape[0]}\n255\n'.encode('ascii'))
        f.write(image.tobytes())
