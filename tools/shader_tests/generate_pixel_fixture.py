#!/usr/bin/env python3
"""Build original synthetic Maxwell PIXLD fixtures and independent GPU readback shaders."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--translator', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
header = bytearray(0x50)
struct.pack_into('<I', header, 0, 5 << 10)  # Fragment stage.
struct.pack_into('<I', header, 0x48, 1)  # Export R0 into target 0.x.
opcode = 0xefe8000000000000 | (7 << 45) | (5 << 31) | (7 << 16) | (255 << 8)
variants = [opcode, (opcode & ~(7 << 31)) | (1 << 31), opcode & ~(7 << 45),
            opcode & ~(255 << 8), opcode | (1 << 20)]
jobs = []
for i, instruction in enumerate(variants):
    code = a.output / f'pixel-{i}.bin'
    # Scheduling word, PIXLD, unconditional EXIT. No game bytes are used.
    code.write_bytes(header + struct.pack('<QQQQ', 0, instruction, 0xe30000000007000f, 0))
    jobs.append(dict(key=f'{i+1:016x}', stage='ps', code=str(code.resolve()),
                     fout={'0': 'uint'}, strict=True, debug=False))
job_path = a.output / 'jobs.json'
job_path.write_text(json.dumps(jobs))
env = dict(os.environ)
env.setdefault('DOTNET_ROOT', '/opt/homebrew/opt/dotnet/libexec')
subprocess.run(['dotnet', str(a.translator), str(job_path), str(a.output)], check=True, env=env)
for i, job in enumerate(jobs):
    notes = json.loads((a.output / (job['key'] + '.res.json')).read_text())['notes']
    assert bool(notes) == (i != 0), (i, notes)  # Unsupported modes must remain visible.
(a.output / 'pixel-id.spv').write_bytes((a.output / '0000000000000001.spv').read_bytes())
for multi in (False, True):
    source = a.output / ('read-ms.comp' if multi else 'read-single.comp')
    source.write_text('''#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=1) uniform usampler2D%s source_image;
layout(binding=2,std430) buffer Output { uint values[]; };
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    uint count=%s;
    for(uint s=0;s<count;s++) values[(p.y*32+p.x)*count+s]=texelFetch(source_image,p,%s).r;
}
''' % ('MS' if multi else '', 'uint(textureSamples(source_image))' if multi else '1u', 'int(s)' if multi else '0'))
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', str(source), '-o', str(source.with_suffix('.spv'))], check=True)
for binary in a.output.glob('*.spv'):
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
print('PASS PIXLD MY_INDEX plus four explicit unsupported-mode/address/predicate cases')
