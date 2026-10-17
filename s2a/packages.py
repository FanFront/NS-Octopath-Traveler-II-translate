"""Metadata-driven package selection and verified, atomic extraction."""
import hashlib
import json
import re
import shutil
import tempfile
from pathlib import Path
from tools.game_package import Hactool, pfs_entries, copy_entry, parse_cnmt


def inventory(source, ht, scratch):
    paths = sorted(p for p in source.iterdir() if p.is_file() and p.suffix.lower() == '.nsp')
    if not paths:
        raise ValueError('No NSP files in input directory')
    for path in paths:
        ht.read_tickets(path)
    records = []
    for i, path in enumerate(paths):
        entries = pfs_entries(path)
        metas = [e for e in entries if e['name'].endswith('.cnmt.nca')]
        if not metas:
            raise ValueError(f'No CNMT in {path.name}')
        for j, entry in enumerate(metas):
            root = scratch / f'{i}-{j}'
            local = root / entry['name']
            copy_entry(path, entry, local)
            ht.run('-t', 'nca', f'--section0dir={root / "meta"}', local)
            files = list((root / 'meta').glob('*.cnmt'))
            if len(files) != 1:
                raise ValueError('Cannot decrypt CNMT with supplied keys')
            data = files[0].read_bytes()
            record = parse_cnmt(data)
            record.update(package=path, entries=entries)
            record['required_version'] = int.from_bytes(data[40:44], 'little') if record['type'] == 0x82 else 0
            records.append(record)
    return records


def select(records, title=None, version=None, include_dlc=True):
    bases = [r for r in records if r['type'] == 0x80 and (not title or r['title_id'] == title)]
    if len(bases) != 1:
        raise ValueError('Expected one base application; use --title-id if directory contains multiple games')
    base = bases[0]; title = base['title_id']
    relevant = [r for r in records if r['application_id'] == title]
    if any(r['type'] not in (0x80, 0x81, 0x82) for r in relevant):
        raise ValueError('Unsupported content metadata type (for example DataPatch); refusing to silently omit it')
    patches = [r for r in relevant if r['type'] == 0x81]
    candidates = [base, *patches]
    seen = set()
    for record in relevant:
        key = (record['title_id'], record['version'])
        if key in seen:
            raise ValueError('Duplicate title/version metadata; remove ambiguous packages')
        seen.add(key)
    if version is not None:
        candidates = [r for r in candidates if r['version'] == version]
    if not candidates:
        raise ValueError('Requested update version is absent')
    selected = max(candidates, key=lambda r: r['version'])
    dlcs = {}
    if include_dlc:
        for record in relevant:
            if record['type'] != 0x82:
                continue
            index = int(record['title_id'], 16) - int(title, 16) - 0x1000
            if not 1 <= index <= 2000:
                raise ValueError('Invalid add-on content index')
            if record['required_version'] > selected['version']:
                raise ValueError('DLC requires a newer application update')
            if index not in dlcs or record['version'] > dlcs[index]['version']:
                dlcs[index] = record
    return base, selected, dict(sorted(dlcs.items()))


def public_record(record):
    return {k: record[k] for k in ('title_id', 'application_id', 'version', 'type', 'contents')}


def selection_id(selection):
    base, selected, dlcs = selection
    payload = [public_record(r) for r in [base, selected, *dlcs.values()]]
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def extract(selection, ht, destination):
    base, selected, dlcs = selection
    identity = selection_id(selection)
    if destination.exists():
        raise FileExistsError(f'Extraction destination already exists: {destination}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    required = sum(c['size'] for r in [base, selected, *dlcs.values()] for c in r['contents']) * 2
    if shutil.disk_usage(destination.parent).free < required:
        raise ValueError(f'Extraction requires approximately {required // 1024**3 + 1} GiB free')
    with tempfile.TemporaryDirectory(prefix='.extract-', dir=destination.parent) as temp:
        temp = Path(temp); stage = temp / 'result'; stage.mkdir()
        def content(record, kind):
            matches = [c for c in record['contents'] if c['type'] == kind]
            if len(matches) != 1 or matches[0]['id_offset'] != 0:
                raise ValueError('Only a single content item with id_offset=0 is supported')
            c = matches[0]; name = c['id'] + '.nca'
            entry = next((e for e in record['entries'] if e['name'] == name), None)
            if not entry or entry['size'] != c['size']:
                raise ValueError('CNMT content missing or size mismatch')
            local = temp / name
            if not local.exists():
                copy_entry(record['package'], entry, local, c['sha256'])
            return local
        program = content(selected, 1)
        args = [f'--exefsdir={stage / "ExeFS"}', f'--romfsdir={stage / "RomFS"}']
        if selected is not base:
            args.append(f'--basenca={content(base, 1)}')
        info = ht.run('-t', 'nca', *args, program)
        if not re.search(rf'Title ID:\s*{base["title_id"]}\b', info, re.I):
            raise ValueError('Decrypted program title does not match CNMT')
        if not (stage / 'ExeFS/main').is_file() or not (stage / 'RomFS').is_dir():
            raise ValueError('Incomplete program extraction')
        control_record = selected if any(c['type'] == 3 for c in selected['contents']) else base
        ht.run('-t', 'nca', f'--romfsdir={stage / "Control"}', content(control_record, 3))
        nacp = (stage / 'Control/control.nacp').read_bytes()
        if len(nacp) < 0x3070:
            raise ValueError('Truncated NACP')
        name = next((nacp[i*0x300:i*0x300+0x200].split(b'\0')[0].decode('utf-8') for i in range(16) if nacp[i*0x300]), base['title_id'])
        if dlcs:
            (stage / 'AddOnContent').mkdir()
        for index, record in dlcs.items():
            print(f'Extracting DLC {index}', flush=True)
            data = content(record, 2)  # CNMT Data includes NCA Data/PublicData.
            dlc_root = stage / 'AddOnContent' / str(index)
            dlc_root.mkdir()
            info = ht.run('-t', 'nca', f'--romfsdir={dlc_root}', data)
            if not re.search(r'Partition Type:\s*RomFS', info) or not re.search(rf'Title ID:\s*{record["title_id"]}\b', info, re.I):
                raise ValueError('DLC has no readable matching RomFS')
        manifest = dict(schema=1, id=identity, title_id=base['title_id'], name=name,
                        version=selected['version'], display_version=nacp[0x3060:0x3070].split(b'\0')[0].decode(),
                        dlc=[dict(index=i, **public_record(r)) for i, r in dlcs.items()])
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        stage.rename(destination)
    return manifest
