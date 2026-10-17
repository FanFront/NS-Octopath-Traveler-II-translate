#!/usr/bin/env python3
"""Prepare offline translation jobs using the game's own NVN assembly metadata."""
import argparse
import json
import pathlib
import re
import struct
from shader_metadata import compute_metadata

TYPES = {'1D': '1d', '2D': '2d', '3D': '3d', 'CUBE': 'cube',
         'SHADOWCUBE': 'cube', 'SHADOW2D': '2d', 'BUFFER': 'buffer',
         'ARRAY1D': '1darray', 'ARRAY2D': '2darray', 'ARRAYCUBE': 'cubearray',
         '2DMS': '2dms', 'ARRAY2DMS': '2dmsarray'}
REGISTER = r'D\d+(?:\.[xyzw])?'


def facts(assembly):
    registers, textures, images = {}, {}, {}
    unresolved = []
    def register(name):
        return name if '.' in name else name + '.x'
    for line in assembly.splitlines():
        load = re.fullmatch(r'LDC\.U64 (' + REGISTER + r'), buf14\[(\d+)\];', line)
        if load:
            registers[register(load[1])] = {int(load[2])}
            continue
        combine = re.fullmatch(r'OR\.U64 (' + REGISTER + r'), (' + REGISTER + r'), (' + REGISTER + r');', line)
        if combine:
            registers[register(combine[1])] = registers.get(register(combine[2]), set()) | registers.get(register(combine[3]), set())
            continue
        handle = re.search(r'handle\((' + REGISTER + r')\)', line)
        if not handle:
            continue
        kind = re.search(r',\s*(' + '|'.join(TYPES) + r')\s*[,;]', line)
        offsets = registers.get(register(handle[1]), set())
        is_image = line.startswith(('STOREIM', 'LOADIM', 'ATOMIM', 'IMQ'))
        # NVN's preserved assembly uses driver bank 14. Its texture/image
        # tables start at 0x148/0x100; final machine bank 2 adds 0x20.
        base, end = (0x100, 0x140) if is_image else (0x148, 0x548)
        slots = {(offset-base)//8 for offset in offsets if base <= offset < end and (offset-base)%8 == 0}
        if not kind or not slots:
            unresolved.append(line)
            continue
        for slot in slots:
            table = images if is_image else textures
            value = {'type': TYPES[kind[1]]} if is_image else TYPES[kind[1]]
            if is_image and line.startswith('ATOMIM'):
                value['fmt'] = 'R32Uint' if '.U32 ' in line else 'R32Sint'
            key = str(slot)
            if key in table:
                previous_type = table[key]['type'] if is_image else table[key]
                if previous_type != TYPES[kind[1]]:
                    raise ValueError(f'Conflicting resource types at slot {slot}: {line}')
                if is_image:
                    table[key].update(value)
            else:
                table[key] = value
    return {'tex': textures, 'img': images}, unresolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shaders', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--captures', type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    jobs, issues, conflicts, adapted_views = [], {}, [], []
    compared = 0
    for control_path in sorted(args.shaders.glob('*.control')):
        key = control_path.stem
        control = control_path.read_bytes()
        code = control_path.with_suffix('.code').read_bytes()
        u = lambda off: struct.unpack_from('<I', control, off)[0]
        assembly = control_path.with_suffix('.asm').read_text(errors='strict')
        inferred, unresolved = facts(assembly)
        if unresolved:
            issues[key] = unresolved
        if args.captures and (observed := args.captures / f'{key}.facts.json').exists():
            runtime = json.loads(observed.read_text())
            for slot, expected in inferred['tex'].items():
                if slot in runtime.get('tex', {}):
                    compared += 1
                    if runtime['tex'][slot] != expected:
                        difference = dict(key=key, slot=slot, assembly=expected, runtime=runtime['tex'][slot])
                        # The runtime exposes a single cube as a one-element
                        # cube array for ARRAYCUBE instructions. Keep the
                        # instruction's type in the translation job.
                        if expected == 'cubearray' and runtime['tex'][slot] == 'cube':
                            adapted_views.append(difference)
                        else:
                            conflicts.append(difference)
        cropped = args.output / f'{key}.bin'
        cropped.write_bytes(code[u(0x708):u(0x700)])
        job = dict(key=key, stage=['vs', 'ps', 'gs', 'tcs', 'tes', 'cs'][u(0x714)],
                   code=str(cropped.resolve()), c1=code[u(0x700):u(0x704)].hex(),
                   debug=True, reduced=False, strict=True, **inferred, attr={}, fout={})
        if job['stage'] == 'cs':
            job.update(compute_metadata(control, assembly))
        jobs.append(job)
    report = dict(shaders=len(jobs), compared_texture_slots=compared,
                  runtime_conflicts=conflicts, adapted_texture_views=adapted_views, unresolved=issues)
    (args.output / 'facts-report.json').write_text(json.dumps(report, indent=2))
    (args.output / 'jobs.json').write_text(json.dumps(jobs))
    print(json.dumps({k: v if k != 'unresolved' else len(v) for k, v in report.items()}, indent=2))
    if conflicts or issues:
        raise SystemExit('Resolve shader facts before deploying the offline cache')


if __name__ == '__main__':
    main()
