#!/usr/bin/env python3
"""Validate NSO modules and inventory their Android compatibility boundary."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import lz4.block


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def checked_slice(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('NSO range out of bounds')
    return data[offset:offset + size]


def load_nso(path):
    data = path.read_bytes()
    if len(data) < 0x100 or data[:4] != b'NSO0':
        raise ValueError('Invalid NSO header')
    flags = u32(data, 0xC)
    segments = []
    for i, name in enumerate(['text', 'rodata', 'data']):
        file_offset, offset, size = struct.unpack_from('<III', data, 0x10 + i * 0x10)
        stored_size = u32(data, 0x60 + i * 4) if flags & (1 << i) else size
        if size > 512 * 1024**2:
            raise ValueError('NSO segment too large')
        raw = checked_slice(data, file_offset, stored_size)
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if len(raw) != size:
            raise ValueError('NSO segment size mismatch')
        if flags & (1 << (i + 3)) and hashlib.sha256(raw).digest() != data[0xA0+i*32:0xC0+i*32]:
            raise ValueError(f'NSO {name} hash mismatch')
        segments.append(dict(name=name, offset=offset, size=size, raw=raw,
                             sha256=hashlib.sha256(raw).hexdigest()))
    bss_size = u32(data, 0x3C)
    image_size = segments[-1]['offset'] + segments[-1]['size'] + bss_size
    if image_size > 1024**3:
        raise ValueError('NSO image too large')
    for a, b in zip(segments, segments[1:]):
        if a['offset'] + a['size'] > b['offset']:
            raise ValueError('NSO segments overlap or are out of order')
    image = bytearray(image_size)
    for segment in segments:
        image[segment['offset']:segment['offset']+segment['size']] = segment['raw']
    return data, image, segments, bss_size


def symbols(header, segments):
    rodata = segments[1]['raw']
    str_offset, str_size, sym_offset, sym_size = struct.unpack_from('<IIII', header, 0x90)
    strings = checked_slice(rodata, str_offset, str_size)
    table = checked_slice(rodata, sym_offset, sym_size)
    if len(table) % 24:
        raise ValueError('Invalid ELF64 symbol table size')
    result = []
    for name_offset, info, other, section, address, size in struct.iter_unpack('<IBBHQQ', table):
        if not name_offset:
            continue
        end = strings.find(b'\0', name_offset)
        if end < 0 or name_offset >= len(strings):
            raise ValueError('Invalid ELF64 symbol name')
        name = strings[name_offset:end].decode('utf-8')
        result.append(dict(name=name, address=address, size=size, section=section,
                           binding=info >> 4, type=info & 15, visibility=other & 3))
    return result


def relocations(image):
    mod = u32(image, 4)
    if checked_slice(image, mod, 4) != b'MOD0':
        raise ValueError('Missing MOD0')
    dynamic = mod + struct.unpack_from('<i', image, mod + 4)[0]
    tags = {}
    for index in range(4096):
        tag, value = struct.unpack('<qQ', checked_slice(image, dynamic + index * 16, 16))
        if tag == 0:
            break
        tags[tag] = value
    else:
        raise ValueError('Unterminated dynamic table')
    counts = Counter()
    for address_tag, size_tag in [(7, 8), (23, 2)]:
        if address_tag not in tags:
            continue
        raw = checked_slice(image, tags[address_tag], tags.get(size_tag, 0))
        if len(raw) % 24:
            raise ValueError('Invalid RELA table size')
        for address, info, addend in struct.iter_unpack('<QQq', raw):
            checked_slice(image, address, 8)
            counts[str(info & 0xFFFFFFFF)] += 1
    return dict(counts), tags


def instruction_sites(segment, include_x18=False):
    sites, counts = [], Counter()
    for index, (word,) in enumerate(struct.iter_unpack('<I', segment['raw'])):
        kind = None
        if word & 0xFFE0001F == 0xD4000001:
            kind = 'svc'
        else:
            kind = {0xD53BD040:'mrs_tpidr_el0', 0xD51BD040:'msr_tpidr_el0',
                    0xD53BD060:'mrs_tpidrro_el0', 0xD53BE020:'mrs_cntpct_el0',
                    0xD53BE040:'mrs_cntvct_el0', 0xD53BE000:'mrs_cntfrq_el0'}.get(word & 0xFFFFFFE0)
        if kind:
            site = dict(offset=segment['offset'] + index * 4, kind=kind)
            if kind == 'svc':
                site['number'] = (word >> 5) & 0xFFFF
            sites.append(site)
            counts[kind] += 1
    if include_x18:
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        decoder.skipdata = True
        for address, size, mnemonic, operands in decoder.disasm_lite(segment['raw'], segment['offset']):
            if re.search(r'\b[wx]18\b', operands):
                counts['x18_references'] += 1
                sites.append(dict(offset=address, kind='x18_reference'))
    return sites, dict(counts)


def audit(exefs, output, include_x18=False):
    output.mkdir(parents=True, exist_ok=True)
    modules, all_symbols = [], {}
    for path in sorted(exefs.iterdir()):
        if not path.is_file():
            continue
        with path.open('rb') as source:
            if source.read(4) != b'NSO0':
                continue
        header, image, segments, bss_size = load_nso(path)
        syms = symbols(header, segments)
        relocs, dynamic = relocations(image)
        sites, instructions = instruction_sites(segments[0], include_x18)
        imports = [s for s in syms if s['section'] == 0]
        exports = [s for s in syms if s['section'] != 0 and s['binding'] in (1, 2)]
        all_symbols[path.name] = syms
        module = dict(name=path.name, build_id=header[0x40:0x60].hex(),
                      sha256=hashlib.sha256(header).hexdigest(), image_size=len(image),
                      bss_size=bss_size, segments=[{k:v for k,v in s.items() if k!='raw'} for s in segments],
                      imports=len(imports), exports=len(exports), relocation_types=relocs,
                      instruction_candidates=instructions, x18_scan_complete=include_x18,
                      init_array=dynamic.get(25), init_array_size=dynamic.get(27))
        modules.append(module)
        (output / f'{path.name}.symbols.json').write_text(json.dumps(syms, indent=2)+'\n')
        (output / f'{path.name}.sites.json').write_text(json.dumps(sites, indent=2)+'\n')
        (output / f'{path.name}.image.bin').write_bytes(image)
        print(json.dumps(module, ensure_ascii=False), flush=True)
    if not modules:
        raise ValueError('No NSO modules found')
    exported = {s['name'] for syms in all_symbols.values() for s in syms
                if s['section'] != 0 and s['binding'] in (1, 2)}
    unresolved = sorted({s['name'] for syms in all_symbols.values() for s in syms
                         if s['section'] == 0 and s['binding'] != 2 and s['name'] not in exported})
    names = sorted({s['name'] for syms in all_symbols.values() for s in syms if s['section']==0})
    demangled = subprocess.run(['c++filt'], input='\n'.join(names)+'\n', capture_output=True, text=True, check=True).stdout
    (output / 'imports.demangled.txt').write_text(demangled)
    report = dict(modules=modules, unresolved_strong_imports=unresolved,
                  executable=False, note='Static candidates, not a reachable-code proof or runtime compatibility verdict')
    (output / 'audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Audit: {output}; unresolved strong imports: {len(unresolved)}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--x18', action='store_true')
    args = parser.parse_args()
    audit(args.exefs, args.output, args.x18)
