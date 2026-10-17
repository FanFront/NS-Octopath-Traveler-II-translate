#!/usr/bin/env python3
"""Translate and validate every extracted shader into a fingerprinted cache set."""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from shader_metadata import validate_compute_binary


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('jobs', type=pathlib.Path)
    p.add_argument('output', type=pathlib.Path)
    p.add_argument('--translator', required=True, type=pathlib.Path)
    p.add_argument('--workers', type=int, default=4)
    a = p.parse_args()
    jobs = json.loads(a.jobs.read_text())
    keys = [j['key'] for j in jobs]
    if not keys or len(set(keys)) != len(keys) or not all(re.fullmatch('[0-9a-f]{16}', k) for k in keys):
        raise SystemExit('Shader jobs must have unique hexadecimal keys')
    a.output.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must not leave an old validation certificate usable.
    (a.output / 'manifest.json').unlink(missing_ok=True)
    env = dict(os.environ)
    compiler = {f.name: digest(f) for f in sorted(a.translator.parent.glob('*.dll'))}
    source_sha = subprocess.check_output(['dotnet', str(a.translator), '--source-sha'], env=env, text=True).strip()
    workers = max(1, min(a.workers, len(jobs)))
    canonical_jobs = []
    for job in jobs:
        job['strict'] = True
        canonical_jobs.append({**job, 'code': digest(pathlib.Path(job['code']))})
    with tempfile.TemporaryDirectory(prefix='ot2-shaders-') as temp:
        def translate(i):
            path = pathlib.Path(temp) / f'{i}.json'
            path.write_text(json.dumps(jobs[i::workers]))
            log = a.output / f'translate-{i}.log'
            with log.open('w') as output:
                r = subprocess.run(['dotnet', str(a.translator), str(path), str(a.output)],
                                   env=env, stdout=output, stderr=subprocess.STDOUT)
            if r.returncode:
                raise RuntimeError(f'Translation failed; see {log}')
        with concurrent.futures.ThreadPoolExecutor(workers) as pool:
            list(pool.map(translate, range(workers)))
    print(f'Translated {len(jobs)} shaders; validating', flush=True)
    def validate(job):
        key = job['key']
        spv, meta = (a.output / (key + suffix) for suffix in ('.spv', '.res.json'))
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True, capture_output=True)
        if job['stage'] == 'cs':
            validate_compute_binary(job, spv.read_bytes())
        reflection = json.loads(meta.read_text())
        if reflection['key'] != key or reflection['stage'] != job['stage'] or reflection['notes']:
            raise ValueError(f'Incomplete shader reflection: {key}')
        return key, {'spv': digest(spv), 'reflection': digest(meta), 'stage': job['stage']}
    with concurrent.futures.ThreadPoolExecutor(workers * 2) as pool:
        entries = dict(pool.map(validate, jobs))
    payload = dict(format=1, target='vulkan1.1', translator_source=source_sha,
                   compiler=compiler, jobs_sha256=hashlib.sha256(json.dumps(canonical_jobs, sort_keys=True).encode()).hexdigest(),
                   shaders=entries)
    payload['id'] = hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()
    temporary = a.output / 'manifest.json.tmp'
    temporary.write_text(json.dumps(payload, indent=2))
    temporary.replace(a.output / 'manifest.json')
    print(f'Validated {len(entries)} shaders; cache {payload["id"]}', flush=True)


if __name__ == '__main__':
    main()
