#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build only the new sparse copy shader; immutable legacy inputs are read-only.

Usage: python3 src/video/gpu/verify/gen_verify_rows.py /path/to/llc HEADER ARTIFACTS
Native compilation is performed by the task's root in the authorized VM.
"""
import hashlib
import importlib.util
import json
import pathlib
import re
import struct
import subprocess
import sys


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    here = pathlib.Path(__file__).resolve().parent
    root = here.parents[3]
    legacy_file = root/'scripts/gen_gpu_convert_shaders.py'
    spec = importlib.util.spec_from_file_location('legacy_shader_reader', legacy_file)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    llc, header, artifacts = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    source = here/'verify_rows.ll'
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in [*here.parent.glob('*.ll'), here.parent/'gpu_shaders.h']}
    version = subprocess.check_output([str(llc), '--version'], text=True)
    if not re.search(r'LLVM version 21\.1\.8\b', version):
        raise ValueError('same LLVM 21.1.8 compiler is required')
    signature = ('define amdgpu_cs void @main(<4 x i32> inreg %yb, <4 x i32> inreg %uvb, '
        '<4 x i32> inreg %out, <4 x i32> inreg %params, i32 inreg %gx, i32 inreg %gy, i32 %tx, i32 %ty)')
    text = source.read_text()
    if signature not in text or 'target triple = "amdgcn--"' not in text:
        raise ValueError('copy shader ABI changed')
    args = [str(llc), '-mcpu=gfx700', '-O2']
    asmrun = subprocess.run(args+['-filetype=asm', str(source), '-o', '-'], capture_output=True, text=True, check=True)
    objrun = subprocess.run(args+['-filetype=obj', str(source), '-o', '-'], capture_output=True, check=True)
    if asmrun.stderr.strip() or objrun.stderr.strip():
        raise ValueError('compiler emitted diagnostics')
    asm, obj = asmrun.stdout, objrun.stdout
    nv = reader.stat(asm, 'NumVgprs')
    ns = reader.stat(asm, 'TotalNumSgprs', 'NumSgprs')
    lds, scratch = reader.stat(asm, 'LDSByteSize'), reader.stat(asm, 'ScratchSize')
    if lds or scratch or re.search(r'^\s*(?:scratch_load|scratch_store|v_writelane_b32|v_readlane_b32)\w*\b', asm, re.M):
        raise ValueError('copy must not require LDS, scratch or register spills')
    for spill in re.findall(r';\s*(?:NumSpilledSGPRs|NumSpilledVGPRs|SGPRSpill|VGPRSpill):\s*(\d+)', asm):
        if int(spill):
            raise ValueError('copy register spill')
    words = reader.text_words(obj)
    r1, r2 = reader.resources(nv, ns, 16, 0, 0, 1, 1)
    padded = words+[0xBF810000]*((-len(words))%64+16)
    lines = ['// Generated solely from verify_rows.ll; legacy arrays unchanged.',
        '#pragma once', '#include <stdint.h>',
        f'#define X4_GPU_VERIFY_ROWS_RSRC1 0x{r1:08X}u',
        f'#define X4_GPU_VERIFY_ROWS_RSRC2 0x{r2:08X}u',
        f'#define X4_GPU_VERIFY_ROWS_NWORDS {len(padded)}u',
        f'static const uint32_t x4_gpu_verify_rows_code[{len(padded)}] = {{']
    lines += ['    '+', '.join(f'0x{w:08X}u' for w in padded[i:i+8])+',' for i in range(0, len(padded), 8)]
    lines += ['};', '']
    artifacts.mkdir(parents=True, exist_ok=True)
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('\n'.join(lines))
    (artifacts/'verify_rows.asm').write_text(asm)
    (artifacts/'verify_rows.o').write_bytes(obj)
    (artifacts/'verify_rows.bin').write_bytes(struct.pack('<%dI' % len(words), *words))
    after = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in [*here.parent.glob('*.ll'), here.parent/'gpu_shaders.h']}
    if before != after or len(before) != 12:
        raise ValueError('immutable shader files changed')
    report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(), 'llc_version': version.splitlines()[0],
        'vgpr': nv, 'sgpr': ns, 'lds': lds, 'scratch': scratch, 'relocations': 0,
        'raw_bytes': len(words)*4, 'padded_bytes': len(padded)*4, 'rsrc1': r1, 'rsrc2': r2,
        'raw_sha256': hashlib.sha256(struct.pack('<%dI' % len(words), *words)).hexdigest(),
        'immutable_legacy_files': before, 'legacy_files_unchanged': True,
        'scope': 'new copy shader compiled; no PS4 performance evidence'}
    (artifacts/'verify_rows_resources.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
