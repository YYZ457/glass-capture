"""Check module ELF, vermagic and actual target Image exports without loading it."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def exports_from_image(image, symbols):
    # ARM64 Image entry branches to primary_entry. Derive mapping instead of
    # assuming the kernel's load address or confusing physical and virtual RAM.
    instruction = struct.unpack_from('<I', image, 4)[0]
    if instruction >> 26 != 0b000101:
        raise ValueError('Unsupported ARM64 Image entry')
    displacement = instruction & 0x3ffffff
    if displacement & (1 << 25):
        displacement -= 1 << 26
    base = int(symbols['primary_entry']['virtual_address'], 16) - (4 + displacement * 4)
    start = int(symbols['_etext']['virtual_address'], 16) - base
    end = int(symbols['__init_begin']['virtual_address'], 16) - base
    candidates = {}
    for position in range(start, end - 12, 4):
        value_rel, name_rel, namespace_rel = struct.unpack_from('<iii', image, position)
        value = position + value_rel
        name = position + 4 + name_rel
        namespace = position + 8 + namespace_rel
        if not (0 <= value < len(image) and 0 < name < len(image) and 0 <= namespace < len(image)):
            continue
        # SHF_MERGE|SHF_STRINGS permits suffix sharing (e.g. kfree inside
        # devm_kfree); an export name need not be preceded by a NUL byte.
        if not (65 <= image[name] <= 90 or 97 <= image[name] <= 122 or image[name] == 95):
            continue
        nul = image.find(b'\0', name, name + 160)
        nsnul = image.find(b'\0', namespace, namespace + 160)
        if nul < 0 or nsnul < 0:
            continue
        raw_name = image[name:nul]
        raw_ns = image[namespace:nsnul]
        if not re.fullmatch(rb'[A-Za-z_][A-Za-z_0-9]*', raw_name) or not re.fullmatch(rb'[A-Za-z_0-9]*', raw_ns):
            continue
        name_text = raw_name.decode('ascii')
        if name_text in symbols and int(symbols[name_text]['virtual_address'], 16) != base + value:
            continue
        candidates[position] = (name_text, raw_ns.decode('ascii'), value)
    # Export arrays consist of contiguous 12-byte records. Exclude isolated
    # instruction/data coincidences; retain only runs with at least 16 entries.
    exports = {}
    runs = []
    for position in sorted(candidates):
        if position - 12 in candidates:
            continue
        run = []
        cursor = position
        while cursor in candidates:
            run.append((cursor, candidates[cursor]))
            cursor += 12
        # Linux's linker sorts each export table by symbol name. Require that
        # structure too: unrelated relative-pointer arrays can otherwise look
        # superficially like exports when scanning a flat Image.
        run_names = [entry[1][0] for entry in run]
        if len(run) < 16 or run_names != sorted(set(run_names)):
            continue
        runs.append({'offset': hex(position), 'entries': len(run)})
        for offset, (name, namespace, value) in run:
            exports[name] = {'record_offset': hex(offset), 'value_offset': hex(value), 'namespace': namespace}
    if len(exports) < 1000:
        raise ValueError(f'Could not validate kernel export arrays: {len(exports)} symbols')
    return exports, runs


def module_details(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected ELF64 little-endian')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[1] != 1 or header[2] != 183:
        raise ValueError('Expected AArch64 relocatable kernel module')
    shoff, shsize, shnum, shstr = header[6], header[11], header[12], header[13]
    if shsize != 64 or shoff + shnum * shsize > len(data):
        raise ValueError('Invalid section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
    def section_bytes(section):
        start, size = section[4], section[5]
        if start + size > len(data):
            raise ValueError('Section outside module')
        return data[start:start + size]
    names = section_bytes(sections[shstr])
    def string(table, offset):
        return table[offset:table.index(b'\0', offset)].decode('ascii')
    named = {string(names, s[0]): s for s in sections}
    modinfo = [s.decode('ascii') for s in section_bytes(named['.modinfo']).split(b'\0') if s]
    symbols = named['.symtab']
    strings = section_bytes(sections[symbols[6]])
    imports = []
    for name, info, other, index, value, size in struct.iter_unpack('<IBBHQQ', section_bytes(symbols)):
        if index == 0 and name:
            imports.append({'name': string(strings, name), 'weak': (info >> 4) == 2})
    return modinfo, imports


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kernel-dir', type=Path, default=Path(r'D:\CodexData\visionkit_zcu102\boot_2022_1\kernel_analysis'))
    parser.add_argument('--module', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = (args.kernel_dir / 'Image').read_bytes()
    if hashlib.sha256(image).hexdigest() != '3288921d868235b4cc536aa86166390cad23bc58bda302444330036836631d02':
        raise ValueError('Target Image changed; re-establish target identity')
    symbols = json.loads((args.kernel_dir / 'symbols.json').read_text(encoding='utf-8'))
    exports, runs = exports_from_image(image, symbols)
    result = {'kernel_sha256': hashlib.sha256(image).hexdigest(), 'export_count': len(exports), 'export_arrays': runs,
              'hardware_loaded': False, 'dma_executed': False}
    if args.module:
        module = args.module.read_bytes()
        modinfo, imports = module_details(module)
        vermagic = next(s.removeprefix('vermagic=') for s in modinfo if s.startswith('vermagic='))
        expected = '5.15.19-xilinx-v2022.1 SMP mod_unload aarch64'
        if vermagic.strip() != expected:
            raise ValueError(f'Vermagic mismatch: {vermagic!r}')
        for symbol in imports:
            symbol['export'] = exports.get(symbol['name'])
        missing = [s['name'] for s in imports if s['export'] is None and not s['weak']]
        namespaces = sorted({s['export']['namespace'] for s in imports if s['export'] and s['export']['namespace']})
        imported_namespaces = {s.removeprefix('import_ns=') for s in modinfo if s.startswith('import_ns=')}
        if set(namespaces) - imported_namespaces:
            raise ValueError(f'Undeclared import namespaces: {namespaces}')
        result.update(module_sha256=hashlib.sha256(module).hexdigest(), vermagic=vermagic,
                      modinfo=modinfo, imports=imports, missing_exports=missing, offline_abi_checks_passed=not missing)
        if missing:
            args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
            raise ValueError(f'Module imports unavailable in target Image: {missing}')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'Validated {len(exports)} actual target-kernel exports; ' + ('module checks passed' if args.module else 'export extraction passed'))


if __name__ == '__main__':
    main()
