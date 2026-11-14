"""GenICam XML → 寄存器清单（给板上 `sg3000-capture camera-regs` 用），再把读回的值解码成名字。

只处理"一个特征直接对应一个寄存器"的常见写法：
    Integer / Enumeration / Boolean / Float / String  --pValue-->  IntReg / MaskedIntReg / FloatReg / StringReg / StructEntry
算出来的特征（IntSwissKnife、Converter）和地址要现算的（pAddress 指向非常数、pIndex 选择器）
会标成 unresolved，不去猜。

用法（电脑上）：
    python host/console/genicam.py CAMERA.zip                 # 打印默认特征的地址表
    python host/console/genicam.py CAMERA.zip --all           # 打印所有能解析的特征
    python host/console/genicam.py CAMERA.zip --board-args    # 打印给 camera-regs 的参数
    python host/console/genicam.py CAMERA.zip --decode LOG    # 把板上 camera-regs 的输出翻译成名字和枚举
"""
import argparse
import io
import re
import struct
import sys
import zipfile
import xml.etree.ElementTree as ET

# 核对相机和采集卡是否一致要看的特征（SFNC 标准名；相机 XML 里没有的会被跳过）
DEFAULT_FEATURES = [
    'DeviceVendorName', 'DeviceModelName', 'DeviceVersion', 'DeviceFirmwareVersion', 'DeviceSerialNumber',
    'DeviceScanType', 'SensorWidth', 'WidthMax', 'Width', 'Height', 'OffsetX',
    'PixelFormat', 'PixelSize', 'PixelDepth',
    'ClConfiguration', 'DeviceTapGeometry', 'ClPixelClockFrequency', 'ClTimeSlotsCount', 'DeviceClockFrequency',
    'AcquisitionMode', 'AcquisitionLineRate', 'AcquisitionLineRateAbs', 'LineRate', 'LinePeriod',
    'ExposureMode', 'ExposureTime', 'ExposureTimeAbs', 'ExposureTimeRaw',
    'TriggerSelector', 'TriggerMode', 'TriggerSource', 'TriggerActivation',
    'Gain', 'GainRaw', 'AnalogGain', 'DigitalGain', 'BlackLevel',
    'BinningHorizontal', 'BinningVertical', 'ReverseX',
    'TestPattern', 'TestImageSelector', 'TestPatternGeneratorSelector',
    'DeviceLinkSpeed', 'DeviceSerialPortBaudRate', 'ClSerialPortBaudRate',
    'UserSetSelector', 'UserSetDefault', 'UserSetDefaultSelector', 'DeviceTemperature',
    # 凌云光 / JAI 这台 16K 相机 XML 里的自有名字（2026-11-14 从相机读出的 XML）
    'CameraLinkClkFrequency', 'InternalLineRate', 'MeasuredLineRate', 'AllExposureTime',
    'MeasuredExposureTime', 'ExposureTimeMode', 'LineTriggerMode', 'LineTriggerSource', 'LineTriggerActivation',
]

REG_TYPES = {'IntReg', 'MaskedIntReg', 'FloatReg', 'StringReg', 'Register', 'StructReg'}
FEATURE_TYPES = {'Integer', 'Enumeration', 'Boolean', 'Float', 'String', 'Command'}
MAX_READ = 256          # 板上 camera-regs 每项最多 256 字节


def load_xml(path_or_bytes):
    """读 .zip（取里面第一个 .xml）或 .xml，返回去掉命名空间的根节点。"""
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, 'rb').read()
    if data[:2] == b'PK':
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.lower().endswith('.xml')]
            if not names:
                raise ValueError('zip 里没有 .xml')
            data = z.read(names[0])
    root = ET.fromstring(data)
    for el in root.iter():
        if isinstance(el.tag, str) and '}' in el.tag:
            el.tag = el.tag.split('}', 1)[1]
    return root


def _int(text):
    text = (text or '').strip()
    return int(text, 16) if text.lower().startswith('0x') else int(text)


class Model:
    def __init__(self, root):
        self.nodes = {}
        for el in root.iter():
            name = el.get('Name')
            if name and el.tag in REG_TYPES | FEATURE_TYPES | {'IntSwissKnife', 'SwissKnife', 'IntConverter',
                                                                'Converter', 'Category', 'Port'}:
                self.nodes.setdefault(name, el)
            if el.tag == 'StructReg':
                for entry in el.findall('StructEntry'):
                    if entry.get('Name'):
                        self.nodes.setdefault(entry.get('Name'), (el, entry))

    # ---- 常数和地址 ----
    def constant(self, name, depth=0):
        """pAddress 之类指向的节点能不能化成常数（Integer 带 <Value>）。不能就 None。"""
        el = self.nodes.get(name)
        if el is None or isinstance(el, tuple) or depth > 8:
            return None
        v = el.find('Value')
        if el.tag == 'Integer' and v is not None:
            return _int(v.text)
        p = el.find('pValue')
        if el.tag == 'Integer' and p is not None:
            return self.constant(p.text.strip(), depth + 1)
        return None

    def address(self, el):
        """寄存器地址 = 所有 <Address> + 所有能化成常数的 <pAddress>。有 pIndex 就算不出确定地址。"""
        if el.find('pIndex') is not None:
            return None, 'indexed (selector)'
        total = 0
        for a in el.findall('Address'):
            total += _int(a.text)
        for pa in el.findall('pAddress'):
            c = self.constant(pa.text.strip())
            if c is None:
                return None, f'pAddress {pa.text.strip()} is computed'
            total += c
        return total, None

    # ---- 特征 → 寄存器 ----
    def resolve(self, name, depth=0):
        """返回 dict：name, type, addr, len, endian, sign, lsb/msb, enum, access, note。解析不了时 addr=None。"""
        out = dict(name=name, type=None, addr=None, len=None, endian='BigEndian', note=None, enum=None,
                   bits=None, reg=None, access=None)
        el = self.nodes.get(name)
        if el is None:
            out['note'] = 'not in XML'
            return out
        if isinstance(el, tuple):            # StructEntry
            reg, entry = el
            return self._register(name, reg, out, entry)
        out['type'] = el.tag
        if el.tag in REG_TYPES:
            return self._register(name, el, out)
        if el.tag == 'Enumeration':
            out['enum'] = {}
            for e in el.findall('EnumEntry'):
                v = e.find('Value')
                if v is not None and e.get('Name'):
                    out['enum'][_int(v.text)] = e.get('Name')
        p = el.find('pValue')
        v = el.find('Value')
        if p is None:
            out['note'] = 'constant' if v is not None else 'no pValue'
            if v is not None and el.tag in ('Integer', 'Float'):
                out['const'] = v.text.strip()
            return out
        target = p.text.strip()
        tel = self.nodes.get(target)
        if tel is None:
            out['note'] = f'pValue {target} not found'
            return out
        if isinstance(tel, tuple) or tel.tag in REG_TYPES:
            r = self.resolve(target, depth + 1)
            r.update(name=name, type=el.tag, enum=out['enum'], reg=target)
            return r
        if tel.tag in FEATURE_TYPES and depth < 6:
            r = self.resolve(target, depth + 1)
            r.update(name=name, type=el.tag, enum=out['enum'] or r.get('enum'), reg=r.get('reg') or target)
            return r
        if tel.tag in ('Converter', 'IntConverter') and depth < 6:
            # 换算型：读它底下的原始寄存器，显示原始值 + 公式（公式不求值，免得算错）
            raw = tel.find('pValue')
            formula = tel.find('FormulaFrom')
            if raw is not None:
                r = self.resolve(raw.text.strip(), depth + 1)
                if r['addr'] is not None:
                    r.update(name=name, type=el.tag, enum=out['enum'],
                             note=f'raw value; {name} = {formula.text.strip() if formula is not None else "?"} '
                                  f'(TO = raw)')
                    return r
        out['note'] = f'computed by {tel.tag} {target}'
        return out

    def _register(self, name, reg, out, entry=None):
        out['reg'] = reg.get('Name')
        out['type'] = out['type'] or reg.tag
        addr, why = self.address(reg)
        length = reg.find('Length')
        if addr is None or length is None:
            out['note'] = why or 'no Length'
            return out
        out['addr'], out['len'] = addr, _int(length.text)
        e = reg.find('Endianess')
        out['endian'] = e.text.strip() if e is not None else 'LittleEndian'
        acc = reg.find('AccessMode')
        out['access'] = acc.text.strip() if acc is not None else None
        sign = reg.find('Sign')
        out['signed'] = sign is not None and sign.text.strip() == 'Signed'
        src = entry if entry is not None else reg
        bit = src.find('Bit')
        lsb, msb = src.find('LSB'), src.find('MSB')
        if bit is not None:
            out['bits'] = (_int(bit.text), _int(bit.text))
        elif lsb is not None and msb is not None:
            out['bits'] = (_int(lsb.text), _int(msb.text))
        if entry is not None and entry.find('Sign') is not None:
            out['signed'] = entry.find('Sign').text.strip() == 'Signed'
        if reg.tag == 'FloatReg':
            out['type'] = out['type'] if out['type'] != 'FloatReg' else 'Float'
            out['float'] = True
        if reg.tag == 'StringReg':
            out['string'] = True
        if out['len'] > MAX_READ:
            out['note'] = f'length {out["len"]} > {MAX_READ}'
            out['addr'] = None
        return out


def decode(info, raw):
    """raw = 板上读回的字节（大端读取顺序原样）。返回人能看的字符串。"""
    if info.get('string') or info.get('type') == 'String':
        return '"' + raw.split(b'\0', 1)[0].decode('ascii', 'replace') + '"'
    data = raw if info['endian'] == 'BigEndian' else raw[::-1]
    if info.get('float'):
        v = struct.unpack('>f' if len(data) == 4 else '>d', data)[0]
        return f'{v:g}'
    value = int.from_bytes(data, 'big')
    nbits = len(data) * 8
    if info.get('bits'):
        lsb, msb = info['bits']
        if info['endian'] == 'BigEndian':      # GenICam：大端寄存器 bit 0 = 最高位
            shift, width = nbits - 1 - lsb, lsb - msb + 1
        else:
            shift, width = lsb, msb - lsb + 1
        value = (value >> shift) & ((1 << width) - 1)
        nbits = width
    if info.get('signed') and value >> (nbits - 1):
        value -= 1 << nbits
    text = f'{value} (0x{value & ((1 << nbits) - 1):x})'
    if info.get('enum') is not None:
        text += ' = ' + info['enum'].get(value, '<not an enum entry>')
    return text


def features(model, names=None, all_features=False):
    if all_features:
        names = sorted(n for n, el in model.nodes.items()
                       if not isinstance(el, tuple) and el.tag in FEATURE_TYPES - {'Command'})
    out = []
    for n in names or DEFAULT_FEATURES:
        info = model.resolve(n)
        if info['note'] == 'not in XML':
            continue
        out.append(info)
    return out


def board_args(infos):
    """camera-regs 的参数：同一个寄存器只读一次。"""
    seen, args = set(), []
    for i in infos:
        if i['addr'] is None:
            continue
        key = (i['addr'], i['len'])
        if key in seen:
            continue
        seen.add(key)
        name = re.sub(r'[^A-Za-z0-9_]', '_', i.get('reg') or i['name'])[:60]
        args.append(f"{name}@0x{i['addr']:x}:{i['len']}")
    return args


LINE = re.compile(r'^CameraReg (\S+) 0x([0-9a-fA-F]+) (?:= (?:0x([0-9a-fA-F]+) \(\d+\)|"[^"]*" hex ([0-9a-fA-F]*))|ERROR ([^\r\n]*))', re.M)


def parse_board_output(text):
    """CameraReg 行 → {(addr): raw bytes}。数值行按读的长度还原不了，只存数值，解码时按寄存器长度补齐。"""
    values = {}
    for m in LINE.finditer(text):
        addr = int(m.group(2), 16)
        if m.group(5) is not None:
            values[addr] = ('error', m.group(5).strip())
        elif m.group(3) is not None:
            values[addr] = ('int', int(m.group(3), 16))
        else:
            values[addr] = ('hex', bytes.fromhex(m.group(4)))
    return values


def decode_board_output(infos, text):
    vals = parse_board_output(text)
    rows = []
    for i in infos:
        if i['addr'] is None:
            rows.append((i['name'], None, 'unresolved: ' + (i['note'] or '?')))
            continue
        v = vals.get(i['addr'])
        if v is None:
            rows.append((i['name'], i['addr'], 'not read'))
            continue
        if v[0] == 'error':
            rows.append((i['name'], i['addr'], 'read failed: ' + v[1]))
            continue
        raw = v[1] if v[0] == 'hex' else v[1].to_bytes(i['len'], 'big')
        rows.append((i['name'], i['addr'], decode(i, raw) + (f"  # {i['note']}" if i['note'] else '')))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('xml', help='相机 XML（.zip 或 .xml）')
    ap.add_argument('--all', action='store_true', help='列出所有能解析的特征')
    ap.add_argument('--feature', action='append', help='只看这些特征（可重复）')
    ap.add_argument('--board-args', action='store_true', help='打印 camera-regs 的参数')
    ap.add_argument('--decode', metavar='LOG', help='板上 camera-regs 输出（文件），翻译成名字')
    a = ap.parse_args(argv)
    model = Model(load_xml(a.xml))
    infos = features(model, a.feature, a.all)
    if a.board_args:
        print(' '.join(board_args(infos)))
        return 0
    if a.decode:
        for name, addr, text in decode_board_output(infos, open(a.decode, encoding='utf-8', errors='replace').read()):
            print(f'{name:32s} {("0x%08x" % addr) if addr is not None else "-":>12s}  {text}')
        return 0
    for i in infos:
        where = f"0x{i['addr']:08x} len {i['len']}" if i['addr'] is not None else 'unresolved: ' + (i['note'] or '?')
        if i['addr'] is not None and i['note']:
            where += '  # ' + i['note']
        extra = ''
        if i.get('bits'):
            extra += f" bits {i['bits'][0]}..{i['bits'][1]}"
        if i.get('enum'):
            extra += ' enum ' + ', '.join(f'{k}={v}' for k, v in sorted(i['enum'].items())[:12])
        print(f"{i['name']:32s} {i['type'] or '':12s} {where}{extra}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
