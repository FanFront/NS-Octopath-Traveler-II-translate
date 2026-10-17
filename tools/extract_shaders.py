#!/usr/bin/env python3
"""Recover OT2's embedded NVN shaders from locally unpacked UE4 assets.

FShaderEntry stores a byte array, its uncompressed length, and a frequency byte.
This tool finds the Switch GLSL header, then accepts only bounded entries that
decompress completely and whose NVN control/code length fields agree. It does
not modify game files and does not require a running Android device.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import lz4.block
from shader_archive import archive_entries

MAGIC = bytes.fromhex('34127698010000001000000020010000')
MAX_CODE = 16 * 1024 * 1024


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def entries(data):
    cursor = 0
    while (marker := data.find(b'LSLG', cursor)) >= 0:
        cursor = marker + 4
        # LZ4 starts with a literal token, optionally followed by length bytes.
        # The uncompressed path has no token. Validate the complete framing;
        # merely finding a magic byte pattern is never enough.
        for prefix in range(0, 16):
            start = marker - prefix - 4
            if start < 0:
                continue
            size = u32(data, start)
            end = start + 4 + size
            if size < 32 or size > MAX_CODE or end + 5 > len(data):
                continue
            expanded, frequency = u32(data, end), data[end + 4]
            if expanded < size or expanded > MAX_CODE or frequency > 5:
                continue
            payload = data[start + 4:end]
            if prefix:
                try:
                    payload = lz4.block.decompress(payload, uncompressed_size=expanded)
                except lz4.block.LZ4BlockError:
                    continue
            if len(payload) != expanded or not payload.startswith(b'LSLG'):
                continue
            yield start, frequency, payload
            cursor = end + 5
            break


def shaders(payload, frequency):
    cursor = 0
    while (at := payload.find(MAGIC, cursor)) >= 0:
        cursor = at + len(MAGIC)
        if at < 8 or at + 0x880 > len(payload):
            continue
        control = payload[at:at + 0x880]
        c1, size, entry = (u32(control, x) for x in (0x700, 0x704, 0x708))
        stage = u32(control, 0x714)
        begin = at - 4 - size
        control_size = u32(payload, at - 4)
        if not (0 <= entry < c1 <= size <= MAX_CODE and begin >= 4):
            continue
        # A few global shaders append 1024 bytes after the declared control
        # section. The serialized container includes that trailer as well.
        if u32(payload, begin - 4) != size or control_size < u32(control, 0x20):
            continue
        if control_size < 0x880 or at + control_size > len(payload):
            continue
        if stage > 5 or [0, 3, 4, 1, 2, 5][stage] != frequency:
            continue
        code = payload[begin:begin + size]
        key = 14695981039346656037
        for byte in control:
            key = ((key ^ byte) * 1099511628211) & ((1 << 64) - 1)
        assembly = payload[at + 0x880:at + control_size].split(b'\0', 1)[0]
        yield f'{key:016x}', control, code, assembly


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('assets', type=pathlib.Path)
    parser.add_argument('global_cache', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--captures', type=pathlib.Path)
    parser.add_argument('--archive', type=pathlib.Path, action='append', default=[],
                        help='UE4 v2 ShaderArchive; may be repeated')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {}
    framed = rejected = files = 0
    paths = [args.global_cache, *sorted(args.assets.rglob('*.uexp')), *args.archive]
    for path in paths:
        data = path.read_bytes()
        files += 1
        reader = archive_entries if path in args.archive else entries
        for offset, frequency, payload in reader(data):
            framed += 1
            recovered = list(shaders(payload, frequency))
            if not recovered:
                rejected += 1
                continue
            for key, control, code, assembly in recovered:
                digest = hashlib.sha256(control + code).hexdigest()
                if key in manifest:
                    if manifest[key]['sha256'] != digest:
                        raise ValueError(f'Shader control key collision: {key}')
                    manifest[key]['occurrences'] += 1
                    continue
                manifest[key] = dict(source=str(path), offset=offset, frequency=frequency,
                                     sha256=digest, code_size=len(code), occurrences=1)
                for suffix, content in (('control', control), ('code', code), ('asm', assembly)):
                    (args.output / f'{key}.{suffix}').write_bytes(content)
        if files % 5000 == 0:
            print(f'{files}/{len(paths)} files, {len(manifest)} unique shaders', flush=True)
    missing = []
    if args.captures:
        for path in args.captures.glob('*.control'):
            key = path.stem
            if key not in manifest:
                missing.append(key)
                continue
            for suffix in ('control', 'code'):
                if (args.output / f'{key}.{suffix}').read_bytes() != (args.captures / f'{key}.{suffix}').read_bytes():
                    raise ValueError(f'Runtime capture differs: {key}.{suffix}')
    report = dict(files=files, framed_entries=framed, rejected_entries=rejected,
                  unique_shaders=len(manifest), missing_captured=sorted(missing), shaders=manifest)
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'shaders'}, indent=2))


if __name__ == '__main__':
    main()
