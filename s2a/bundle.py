"""A bounded streaming ZIP payload with a manifest pinned by the generated APK."""
import hashlib
import json
import os
import re
import zipfile
from pathlib import Path, PurePosixPath

BLOCK = 4 * 1024 * 1024


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    return bool(name) and '\\' not in name and not path.is_absolute() and path.as_posix() == name and all(p not in ('', '.', '..') for p in name.split('/'))


def shader_files(root):
    manifest = json.loads((root / 'manifest.json').read_text())
    identity = manifest.pop('id')
    if sha_json(manifest) != identity or manifest.get('format') != 1 or manifest.get('target') != 'vulkan1.1' or not manifest.get('shaders'):
        raise ValueError('Invalid shader manifest identity')
    paths = [root / 'manifest.json']
    for key, item in sorted(manifest['shaders'].items()):
        if not re.fullmatch('[0-9a-f]{16}', key):
            raise ValueError('Invalid shader key')
        for field, suffix in [('spv', '.spv'), ('reflection', '.res.json')]:
            path = root / (key + suffix)
            if path.is_symlink() or sha256(path) != item[field]:
                raise ValueError(f'Shader cache validation failed: {path.name}')
            paths.append(path)
    return paths


def sha_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def build(game, shaders, output, profile):
    files = []
    for part in ('ExeFS', 'RomFS', 'AddOnContent'):
        for path in sorted((game / part).rglob('*')):
            if path.is_symlink():
                raise ValueError('Symbolic links are not allowed in game content')
            if path.is_file():
                files.append((f'game/{path.relative_to(game).as_posix()}', path))
    files.append(('game/game-build.json', game / 'game-build.json'))
    files.extend(('shaders/' + p.name, p) for p in shader_files(shaders))
    records = [dict(path=name, size=path.stat().st_size, sha256=sha256(path)) for name, path in files]
    directories = [f'game/AddOnContent/{r["index"]}' for r in profile.get('dlc', [])]
    index = dict(schema=1, title_id=profile['title_id'], total_bytes=sum(r['size'] for r in records), directories=directories, files=records)
    raw = json.dumps(index, ensure_ascii=True, separators=(',', ':')).encode()
    if len(raw) > 32 * 1024 * 1024:
        raise ValueError('Bundle index exceeds 32 MiB')
    identity = hashlib.sha256(raw).hexdigest()
    temporary = output.with_suffix('.partial')
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            z.writestr('index.json', raw)
            for record, (name, path) in zip(records, files):
                if not safe_name(name):
                    raise ValueError('Invalid bundle path')
                digest = hashlib.sha256()
                with path.open('rb') as source, z.open(name, 'w', force_zip64=record['size'] >= zipfile.ZIP64_LIMIT) as target:
                    while block := source.read(BLOCK):
                        digest.update(block); target.write(block)
                if digest.hexdigest() != record['sha256']:
                    raise ValueError('Source changed during packaging')
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(schema=1, title_id=profile['title_id'], bundle_id=identity, total_bytes=index['total_bytes'], name=profile['name'])


def verify(path, expected=None):
    with zipfile.ZipFile(path) as z:
        entries = z.infolist()
        if not entries or entries[0].filename != 'index.json' or entries[0].file_size > 32 * 1024 * 1024:
            raise ValueError('Missing or oversized bundle index')
        raw = z.read(entries[0]); identity = hashlib.sha256(raw).hexdigest()
        if expected and identity != expected:
            raise ValueError('Bundle does not match APK')
        index = json.loads(raw)
        if index.get('schema') != 1 or len(entries) != len(index['files']) + 1:
            raise ValueError('Invalid bundle schema or unexpected entries')
        for directory in index.get('directories', []):
            if not re.fullmatch(r'game/AddOnContent/[1-9][0-9]{0,3}', directory) or int(directory.rsplit('/', 1)[1]) > 2000:
                raise ValueError('Invalid DLC directory')
        names = set(); total = 0
        for entry, record in zip(entries[1:], index['files']):
            name = record['path']
            if not safe_name(name) or name in names or entry.filename != name or entry.file_size != record['size']:
                raise ValueError('Unsafe, duplicate or mismatched bundle entry')
            names.add(name); total += entry.file_size
            with z.open(entry) as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != record['sha256']:
                    raise ValueError('Corrupt bundle entry')
        if total != index['total_bytes']:
            raise ValueError('Bundle total size mismatch')
    return identity
