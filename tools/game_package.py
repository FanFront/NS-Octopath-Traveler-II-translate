#!/usr/bin/env python3
"""Inspect and extract local application packages. Never emit keys or raw hactool output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile



def pfs_entries(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        header = f.read(16)
        if len(header) != 16:
            raise ValueError('Truncated PFS0 header')
        magic, count, strings_size, _ = struct.unpack('<4sIII', header)
        if magic != b'PFS0' or count > 10000 or strings_size > 16 * 1024 * 1024:
            raise ValueError('Invalid PFS0 header')
        data_start = 16 + 24 * count + strings_size
        if data_start > size:
            raise ValueError('PFS0 table exceeds package')
        table, strings = f.read(count * 24), f.read(strings_size)
    entries, names, spans = [], set(), []
    for i in range(count):
        offset, length, name_offset, _ = struct.unpack_from('<QQII', table, i * 24)
        end = strings.find(b'\0', name_offset)
        if name_offset >= len(strings) or end < 0:
            raise ValueError('Invalid PFS0 name')
        name = strings[name_offset:end].decode('utf-8')
        if not name or name in ('.', '..') or '/' in name or '\\' in name or name in names:
            raise ValueError('Unsafe or duplicate PFS0 name')
        if offset + length > size - data_start:
            raise ValueError('PFS0 entry exceeds package')
        names.add(name)
        if length:
            spans.append((offset, offset + length))
        entries.append(dict(name=name, offset=data_start + offset, size=length))
    spans.sort()
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError('Overlapping PFS0 entries')
    return entries


def copy_entry(package, entry, destination, expected_hash=None):
    """Stream into a private temporary file; publish only after complete validation."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise FileExistsError(destination)
    digest = hashlib.sha256()
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as out:
            temporary = Path(out.name)
            with package.open('rb') as source:
                source.seek(entry['offset'])
                remaining = entry['size']
                while remaining:
                    block = source.read(min(8 * 1024 * 1024, remaining))
                    if not block:
                        raise ValueError('Truncated package payload')
                    out.write(block)
                    digest.update(block)
                    remaining -= len(block)
        if expected_hash and digest.hexdigest() != expected_hash:
            raise ValueError('Content SHA256 differs from CNMT')
        os.link(temporary, destination)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)
    return digest.hexdigest()


def parse_cnmt(data):
    if len(data) < 32:
        raise ValueError('Truncated CNMT')
    title, version, kind, platform, extended_size, count = struct.unpack_from('<QIBBHH', data)
    start = 32 + extended_size
    if start + count * 56 > len(data) or platform != 0:
        raise ValueError('Unsupported or truncated CNMT')
    contents = []
    for i in range(count):
        item = data[start + i * 56:start + (i + 1) * 56]
        contents.append(dict(sha256=item[:32].hex(), id=item[32:48].hex(),
                             size=int.from_bytes(item[48:54], 'little'),
                             type=item[54], id_offset=item[55]))
    application = title if kind == 0x80 else (
        struct.unpack_from('<Q', data, 32)[0] if extended_size >= 8 else 0)
    return dict(title_id=f'{title:016x}', application_id=f'{application:016x}',
                version=version, type=kind, contents=contents)


class Hactool:
    def __init__(self, executable, keys):
        self.executable, self.keys = executable, keys
        self.titlekeys = {}

    def read_tickets(self, package):
        for entry in pfs_entries(package):
            if not entry['name'].endswith('.tik'):
                continue
            if entry['size'] > 65536:
                raise ValueError('Ticket exceeds size limit')
            with package.open('rb') as f:
                f.seek(entry['offset'])
                data = f.read(entry['size'])
            signature = int.from_bytes(data[:4], 'little')
            base = {0x10000:0x240, 0x10001:0x140, 0x10002:0x80,
                    0x10003:0x240, 0x10004:0x140, 0x10005:0x80, 0x10006:0x40}.get(signature)
            if base is None or len(data) < base + 0x170:
                raise ValueError('Unsupported ticket signature or truncated ticket')
            if data[base + 0x140] != 2 or data[base + 0x141] != 0:
                raise ValueError('Requires an exported common ticket; personalized tickets unsupported')
            rights, key = data[base+0x160:base+0x170].hex(), data[base+0x40:base+0x50].hex()
            if entry['name'] != rights + '.tik':
                raise ValueError('Ticket rights ID does not match its container entry')
            if rights in self.titlekeys and self.titlekeys[rights] != key:
                raise ValueError('Conflicting tickets for the same rights ID')
            self.titlekeys[rights] = key

    def _key_material(self):
        # Only NCA keys; unrelated console-specific derivation fields are not inputs.
        # Pipe to the child: do not copy secrets into temporary files or argv.
        selected = []
        for line in self.keys.read_text().splitlines():
            if '=' not in line:
                continue
            name, value = [s.strip() for s in line.split('=', 1)]
            relevant = (name == 'header_key' or
                        re.fullmatch(r'(master_key|titlekek|key_area_key_(application|ocean|system))_[0-9a-f]{2}', name) or
                        name in ('aes_kek_generation_source', 'aes_key_generation_source',
                                 'key_area_key_application_source', 'key_area_key_ocean_source',
                                 'key_area_key_system_source', 'titlekek_source'))
            if relevant:
                length = 64 if name == 'header_key' else 32
                if not re.fullmatch('[0-9a-fA-F]{' + str(length) + '}', value):
                    raise ValueError(f'Invalid local key length for {name}; value withheld')
                selected.append(f'{name} = {value}\n')
        return ''.join(selected).encode()

    def run(self, *args):
        # Capture in memory only: even error paths in upstream can contain key material.
        material = self._key_material()
        read_fd, write_fd = os.pipe()
        title_read, title_write = os.pipe()
        try:
            environment = dict(os.environ, OT2_TITLEKEYS_FILE=f'/dev/fd/{title_read}')
            process = subprocess.Popen([str(self.executable), '-k', f'/dev/fd/{read_fd}',
                                        '--disablekeywarns', '--suppresskeys', *map(str, args)],
                                       pass_fds=(read_fd, title_read), env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            os.close(read_fd)
            read_fd = -1
            with os.fdopen(write_fd, 'wb') as pipe:
                write_fd = -1
                pipe.write(material)
            os.close(title_read)
            title_read = -1
            with os.fdopen(title_write, 'wb') as pipe:
                title_write = -1
                pipe.write(''.join(f'{k} = {v}\n' for k, v in self.titlekeys.items()).encode())
            stdout, stderr = process.communicate()
            if process.returncode:
                raise RuntimeError(f'hactool failed (exit {process.returncode}); raw output withheld')
            return stdout.decode('utf-8', errors='replace')
        finally:
            for fd in (read_fd, write_fd, title_read, title_write):
                if fd >= 0:
                    os.close(fd)

