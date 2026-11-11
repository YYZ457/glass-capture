"""verify/ 共用的 PGM（P5，8 位）读取。

用 python -I 运行时脚本所在目录不在 sys.path 里，所以各脚本自己把本目录加进去再 import。
"""
import numpy as np


def read_pgm(path):
    """读 P5 PGM，返回 (height, width) 的 uint8 数组。只支持 maxval 255。"""
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":                     # 注释行
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1                                              # 头后面恰好一个空白字符
    if fields[0] != b"P5":
        raise ValueError(f"{path}: not a P5 PGM")
    width, height, maxval = (int(f) for f in fields[1:])
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval}, only 255 supported")
    if len(data) - pos != width * height:
        raise ValueError(f"{path}: expected {width * height} pixel bytes, got {len(data) - pos}")
    return np.frombuffer(data, dtype=np.uint8, offset=pos).reshape(height, width)
