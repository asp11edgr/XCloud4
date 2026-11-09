#!/usr/bin/env python3
"""Compile and validate immutable conversion and optional Lab quality shaders.

Usage: python3 scripts/gen_gpu_convert_shaders.py /official/path/llc src/video/gpu build/gpu_shaders.h
Generated .asm/.o/.bin and shader-resources.json accompany the header.
This is packaging/ABI validation, not a claim of PS4 execution.

SPDX-License-Identifier: MIT
ELF extraction/resource logic adapted from VCFG classic/gen_shaders.py,
commit 955d3f8a8afed278d7acdd8900635797795999ef.
Copyright (c) 2026 VCFG contributors
Copyright (c) 2026 XCloud4 contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess


SHADERS_1080 = ('original', 'smooth0', 'smooth1', 'smooth2')
SHADERS_720 = ('original720', 'smooth720_0', 'smooth720_1', 'smooth720_2')
SHADERS_LEGACY = SHADERS_1080 + SHADERS_720
SHADERS_QUALITY = ('quality_direct', 'quality_prepare', 'quality_finish')
SHADERS = SHADERS_LEGACY + SHADERS_QUALITY
# TAREA24 R3 base: 04df7b720b8b2f68f566c42686ff19010aeb69c67e9243de8652e071b53424d5.
# These anchors are independent of the header being regenerated. A modified
# checked-in baseline must not silently become the next immutable baseline.
LEGACY_SHA256 = {
    'original': ('3a758efbf2db99da6a974456c43e5939168ab0e845fd777e5bb7d59dc659dda0', '855255f88edc130a22927b60594f330392940956bf6082478ed2beb741ca0521'),
    'smooth0': ('5c308d9b6f9e2430d602161952e7237c17fa986068347d5103ba8c5d2e1d667f', '25b0949598523fd810111bc28f50ccbe418c010839774458d8774f3295c09c64'),
    'smooth1': ('f616d54f27cafced86d558f8182a11721ba5409c5ed79e92f93fa1a5111feff8', '9c9ce84ee7d56ef102f6b19335e26c69027da0f1ded01395100461e9c2fec554'),
    'smooth2': ('3aeed0cff4acbf66a96207b69d735484fbeda623c5d67e42114b270082aaa65c', '60f4cd7ec7a87b4567d0afa8efe7c5d4c9b91fdbc05fd483d3a53b060c9dc6cc'),
    'original720': ('2b3b3dec277b610ceb323cd7a8796d02ad9184d90d8fc7e0af6076976bb557d0', '8e02940c7aae5b7ff4d373c6af9e6bb6da8fd55dd270c2655bebae446d99d9c6'),
    'smooth720_0': ('5182e2994c31c4ce5a7d72dd8329c6d8f1d804453a018639c0083676e64d6dd5', '7d5d01408d0936ee0527e91517661aabce6fcdd7d3a93ddc63951ed2f9cb01f5'),
    'smooth720_1': ('b7519a014ed4858181de74f0e5cd15501ae1cf66b0d285340d5573cfd91b82de', 'd432978726f0965f8a2bc4b9a9f771ca176e6bc15a9a513d6554a516509336fd'),
    'smooth720_2': ('2efea784f62dce84e275479d7dbb1fef129c00df4c287f5555741d90446fa72f', '9598880b8063a6e0686f491396fb8b023eea3d73fa02d0a89ded3723146be845'),
}
LEGACY_MACROS_SHA256 = {
    'original': '397715a6c2038bab6afdb4795ac45cac5ecc80a97afb09bd007e4533312073e4',
    'smooth0': '4d0992e8adc98dc66a0f3af22c6a32a3a38c448ca5be1b92e70cbda31ab9107f',
    'smooth1': '7c8518a6cc37ed2ff1b37e3bcd6aea2b7631d93383606afe6aad5d3a6110a2bd',
    'smooth2': '941a2c5e136d4bfaf944cf262218d321b3cc3c55b53be82e47f536f50e4262b5',
    'original720': 'b4e8be449fd9f163a28df329cde477554d3c2b94c5153d292c3b823cdcae73f5',
    'smooth720_0': '3bdce3d387cf9319b7dcf59a60c2c411b364f7103a9ee49d7f8f6dfbdbb4ac43',
    'smooth720_1': 'b909567c45fe2cf6f7d7829f4406385e530a1abea877f25842685c68bdc7a890',
    'smooth720_2': 'f280a234647e8a0b82a9132edf751287319104487c987eaf0acea9854816ae44',
}


def immutable_baseline(sdir):
    """Verify all eight R3 sources, machine arrays and resource macro blocks."""
    baseline=(sdir/'gpu_shaders.h').read_text()
    blocks={}
    for name in SHADERS_LEGACY:
        source_sha,array_sha=LEGACY_SHA256[name]
        if hashlib.sha256((sdir/(name+'.ll')).read_bytes()).hexdigest()!=source_sha:
            raise ValueError(name+': immutable TAREA24 R3 source differs')
        up='X4_GPU_'+name.upper()
        macros=[line for line in baseline.splitlines() if line.startswith('#define '+up+'_')]
        if hashlib.sha256(('\n'.join(macros)+'\n').encode()).hexdigest()!=LEGACY_MACROS_SHA256[name]:
            raise ValueError(name+': immutable TAREA24 R3 resource macros differ')
        matches=list(re.finditer(r'static const uint32_t x4_gpu_'+name+r'_code\[(\d+)\] = \{(.*?)\};',baseline,re.S))
        if len(matches)!=1:
            raise ValueError(name+': expected exactly one immutable array')
        match=matches[0]
        words=[int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{8})u',match[2])]
        if len(words)!=int(match[1]) or hashlib.sha256(struct.pack('<%dI'%len(words),*words)).hexdigest()!=array_sha:
            raise ValueError(name+': immutable TAREA24 R3 machine array differs')
        starts=list(re.finditer(r'^#define '+up+r'_RSRC1 ',baseline,re.M))
        if len(starts)!=1 or starts[0].start()>=match.start():
            raise ValueError(name+': malformed immutable shader block')
        blocks[name]=baseline[starts[0].start():match.end()]
    return blocks


def quality_assembly(asm):
    """Strict arithmetic must remain reproducible by the CPU oracle."""
    # Integer MAD remains valid; only floating-point contraction and hardware
    # reciprocal/sqrt approximations are banned. Bitcast/Newton arithmetic is
    # kept explicitly in the IR and evaluated by both backends.
    forbidden=r'^\s*(?:v_(?:fma|fmac|mad|mac)\w*_f(?:16|32|64)\w*|v_(?:rcp|rsq|sqrt)\w*|v_div_(?:scale|fmas)\w*)\b'
    if re.search(forbidden,asm,re.M):
        raise ValueError('quality shader contains contracted or approximate floating-point ISA')


def quality_ir(src):
    code=re.sub(r';[^\n]*','',src)
    # The literal attribute is a prohibition, not the per-instruction
    # `contract` flag. Permit only this exact safe spelling/value.
    code=re.sub(r'"fp-contract"\s*=\s*"off"','',code)
    if re.search(r'\b(?:fast|reassoc|contract|afn|arcp|nnan|ninf|nsz)\b',code):
        raise ValueError('fast/contract/approximate flags in quality IR')
    unsafe=r'"(?:unsafe-fp-math|no-nans-fp-math|no-infs-fp-math|no-signed-zeros-fp-math|approx-func-fp-math)"\s*=\s*"true"'
    if re.search(unsafe,code):
        raise ValueError('unsafe floating-point assumptions in quality IR')

def text_words(obj):
    """Accept only ELF64 AMDGPU gfx700, one entry at zero, no loader work."""
    def region(off, size):
        if off < 0 or size < 0 or off > len(obj) or size > len(obj)-off:
            raise ValueError('ELF region outside file')
        return obj[off:off+size]
    region(0, 64)
    if obj[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('expected ELF64 little-endian v1')
    typ, machine, version = struct.unpack_from('<HHI', obj, 16)
    entry = struct.unpack_from('<Q', obj, 24)[0]
    flags = struct.unpack_from('<I', obj, 48)[0]
    if (typ, machine, version) != (1, 224, 1) or (flags & 0xff) != 0x22:
        raise ValueError('expected relocatable AMDGPU gfx700 ELF')
    if entry:
        raise ValueError('ELF entry must be zero')
    shoff = struct.unpack_from('<Q', obj, 40)[0]
    ehsize, phstride, phcount, stride, count, strings = struct.unpack_from('<HHHHHH', obj, 52)
    if ehsize != 64 or stride != 64 or not 0 < strings < count <= 128 or phcount or phstride:
        raise ValueError('unsupported ELF section/program table')
    region(shoff, stride*count)
    secs = [struct.unpack_from('<IIQQQQIIQQ', obj, shoff+i*stride) for i in range(count)]
    for sec in secs[1:]:
        if sec[1] != 8:
            region(sec[4], sec[5])
    st = secs[strings]
    if st[1] != 3:
        raise ValueError('invalid section string table')
    names = region(st[4], st[5])
    def name(table, offset):
        if offset >= len(table) or table.find(b'\0', offset) < 0:
            raise ValueError('invalid ELF string offset')
        return table[offset:table.index(b'\0', offset)].decode('ascii')
    section_names = [name(names, s[0]) for s in secs]
    texts = [i for i,n in enumerate(section_names) if n == '.text']
    if len(texts) != 1:
        raise ValueError('expected one .text')
    ti = texts[0]; text = secs[ti]
    if text[1] != 1 or text[2]&6 != 6 or not text[5] or text[5]%4:
        raise ValueError('invalid executable .text')
    # Only these nonallocated metadata sections may accompany the raw code.
    # .AMDGPU.config describes resources for non-HSA ELF and is never copied.
    allowed_metadata = {'', '.symtab', '.strtab', '.shstrtab', '.note.GNU-stack',
                        '.AMDGPU.config', '.AMDGPU.gpr_maximums', '.note', '.comment'}
    for i,s in enumerate(secs):
        if s[1] in (4,9) and s[5]:
            raise ValueError('relocations require a linker; refusing raw shader')
        if i != ti and (s[2]&2 and s[5] or section_names[i] not in allowed_metadata):
            raise ValueError('extra section not supported by raw shader loader: '+section_names[i])
        if i != ti and s[2]&4:
            raise ValueError('extra executable section')
        if section_names[i]=='.AMDGPU.gpr_maximums' and (s[1]!=1 or s[2] or s[5]):
            # LLVM 21.1.8 AMDGPUAsmPrinter.cpp:455-466 creates this empty
            # flags-zero PROGBITS metadata section; no runtime data is copied.
            raise ValueError('unsupported nonempty/allocated GPR metadata')
        if section_names[i]=='.note' and (s[1]!=7 or s[2] or not 12<=s[5]<=4096):
            # LLVM 21.1.8 AMDGPUTargetStreamer.cpp:639-662,844-858: ISA note.
            # The non-HSA path uses flags zero; HSA allocated notes stay banned.
            raise ValueError('unsupported ISA note metadata')
    mains = []
    symbols = 0
    for s in secs:
        if s[1] != 2:
            continue
        symbols += 1
        if s[9] != 24 or s[5]%24 or not 0 < s[6] < count or secs[s[6]][1] != 3:
            raise ValueError('invalid ELF symbol table')
        stringsec=secs[s[6]]; tab=region(stringsec[4],stringsec[5])
        for off in range(s[4],s[4]+s[5],24):
            nm,info,_,section,value,size=struct.unpack_from('<IBBHQQ',obj,off)
            symname=name(tab,nm)
            if symname=='main':
                mains.append((section,value,size,info&15))
            if info&15==2 and section==ti and symname!='main':
                raise ValueError('out-of-line helper requires separate ABI validation')
            if symname and section==0 and info>>4:
                raise ValueError('undefined external symbol: '+symname)
    if symbols!=1 or len(mains)!=1 or mains[0][0]!=ti or mains[0][1]!=0 or mains[0][2]>text[5] or mains[0][3]!=2:
        raise ValueError('main must be the sole function at .text offset zero')
    if not mains[0][2]:
        raise ValueError('empty main')
    raw=region(text[4],text[5])
    return list(struct.unpack('<%dI'%(len(raw)//4),raw))


def stat(asm, *names):
    for n in names:
        m=re.search(r';\s*%s:\s*(\d+)'%re.escape(n),asm)
        if m:
            return int(m[1])
    raise ValueError('missing LLVM statistic: '+', '.join(names))


def resources(nv, ns, nuser, lds, scratch, group_y, local_y):
    if scratch:
        raise ValueError('scratch/spills require an unimplemented scratch ring')
    if not 1<=nv<=256 or not 1<=ns<=104 or nuser != 16 or not 0<=lds<=32768:
        raise ValueError('resource count exceeds conservative gfx700 limits')
    if ((ns+7)//8+1)*8 > 104:
        raise ValueError('conservative extra SGPR granule exceeds gfx700 limit')
    # Preserve VCFG FLOAT_MODE/DX10_CLAMP and deliberate extra SGPR granule.
    r1=(nv-1)//4 | ((ns+7)//8)<<6 | (0xc0<<12) | (1<<21)
    # USER_SGPR, TGID_X_EN, optional TGID_Y_EN, TIDIG_COMP_CNT (v0/v1), LDS.
    r2=(nuser<<1) | (1<<7) | (int(group_y)<<8) | (int(local_y)<<11) | (((lds+511)//512)<<15)
    return r1,r2


def abi(name, src):
    signature = '<4 x i32> inreg %yb, <4 x i32> inreg %uvb, <4 x i32> inreg %out, <4 x i32> inreg %params, i32 inreg %gx, i32 inreg %gy, i32 %tx, i32 %ty'
    expected={name: (signature,16,1,1,8,8) for name in SHADERS}
    if name not in expected:
        raise ValueError('unexpected shader name: '+name)
    signature,nuser,group_y,local_y,nx,ny=expected[name]
    if 'define amdgpu_cs void @main('+signature+')' not in src:
        raise ValueError(name+': entry signature differs from the app ABI')
    if 'target triple = "amdgcn--"' not in src:
        raise ValueError('expected non-HSA amdgcn-- shader ABI')
    for key,value in [('user_sgprs',nuser),('tgid_y',group_y),('local_y',local_y),('threads_x',nx),('threads_y',ny)]:
        matches=re.findall(r';\s*'+key+r'=(\d+)\b',src)
        if matches != [str(value)]:
            raise ValueError(name+': missing or inconsistent '+key)
    return nuser,group_y,local_y,nx,ny


def build(llc, sdir, out):
    sources=sorted(sdir.glob('*.ll'))
    if {s.stem for s in sources} != set(SHADERS):
        raise ValueError('expected exactly eight immutable conversion and three quality shader sources')
    version=subprocess.run([llc,'--version'],capture_output=True,text=True,check=True).stdout
    if not re.search(r'LLVM version 21\.1\.8\b',version):
        raise ValueError('the immutable 1080p baseline requires the same LLVM 21.1.8 compiler')
    if not re.search(r'^\s+amdgcn\s+-',version,re.M):
        raise ValueError('llc has no AMDGPU target')
    # All legacy compile flags and header blocks stay unchanged. Quality
    # options are applied only to the three new kernels.
    immutable=immutable_baseline(sdir)
    help_text=subprocess.run([llc,'--help-hidden'],capture_output=True,text=True,check=True).stdout
    if 'fp-contract' not in help_text:
        raise ValueError('strict quality arithmetic requires llc -fp-contract=off support')
    out.parent.mkdir(parents=True,exist_ok=True)
    lines=['// Generated by scripts/gen_gpu_convert_shaders.py; do not edit.','#pragma once','#include <stdint.h>','']
    for name in sorted(SHADERS_LEGACY):
        lines += [immutable[name],'']
    report={'llc':str(pathlib.Path(llc).resolve()),'llc_version':version,'target':'amdgcn--','mcpu':'gfx700','flags':['-mcpu=gfx700','-O2'],'quality_flags':['-fp-contract=off'],'immutable_r3_checks':24,'shaders':{}}
    for ll in sources:
        name=ll.stem; src=ll.read_text()
        nuser,gy,ly,nx,ny=abi(name,src)
        args=[llc,'-mcpu=gfx700','-O2']
        if name in SHADERS_QUALITY:
            quality_ir(src)
            args+=['-fp-contract=off']
        asmrun=subprocess.run(args+['-filetype=asm',str(ll),'-o','-'],capture_output=True,text=True,check=True)
        objrun=subprocess.run(args+['-filetype=obj',str(ll),'-o','-'],capture_output=True,check=True)
        if asmrun.stderr.strip() or objrun.stderr.strip():
            raise ValueError(name+': llc emitted diagnostics: '+asmrun.stderr+objrun.stderr.decode())
        asm=asmrun.stdout;obj=objrun.stdout
        if name in SHADERS_QUALITY:
            quality_assembly(asm)
        nv=stat(asm,'NumVgprs'); ns=stat(asm,'TotalNumSgprs','NumSgprs')
        scratch=stat(asm,'ScratchSize'); lds=stat(asm,'LDSByteSize')
        for count in re.findall(r';\s*(?:NumSpilledSGPRs|NumSpilledVGPRs|SGPRSpill|VGPRSpill):\s*(\d+)',asm):
            if int(count):
                raise ValueError(name+': register spill reported')
        if re.search(r'^\s*(?:scratch_load|scratch_store|v_writelane_b32|v_readlane_b32)\w*\b',asm,re.M):
            # These kernels have no legitimate lane-register transfers;
            # a lane transfer can hide an SGPR-to-VGPR spill with ScratchSize=0.
            raise ValueError(name+': scratch or register-spill lane instruction detected')
        r1,r2=resources(nv,ns,nuser,lds,scratch,gy,ly)
        if lds:
            raise ValueError(name+': unexpected LDS allocation')
        words=text_words(obj)
        raw=struct.pack('<%dI'%len(words),*words)
        # 256-byte entry alignment and terminal S_ENDPGM padding for prefetch.
        padded=words+[0xBF810000]*((-len(words))%64+16)
        if name in immutable and hashlib.sha256(struct.pack('<%dI'%len(padded),*padded)).hexdigest()!=LEGACY_SHA256[name][1]:
            raise ValueError(name+': regenerated array differs from immutable TAREA24 R3')
        up='X4_GPU_'+name.upper()
        generated=[f'#define {up}_RSRC1 0x{r1:08X}u',f'#define {up}_RSRC2 0x{r2:08X}u',f'#define {up}_NWORDS {len(padded)}u',f'#define {up}_VGPR {nv}u',f'#define {up}_SGPR {ns}u',f'#define {up}_LDS {lds}u',f'#define {up}_THREADS_X {nx}u',f'#define {up}_THREADS_Y {ny}u',f'static const uint32_t x4_gpu_{name}_code[{len(padded)}] = {{']
        generated += ['    '+', '.join(f'0x{w:08X}u' for w in padded[i:i+8])+',' for i in range(0,len(padded),8)]
        generated += ['};']
        if name in immutable:
            if '\n'.join(generated)!=immutable[name]:
                raise ValueError(name+': regenerated resource/array block differs from TAREA24 R3')
        else:
            lines += generated+['']
        (out.parent/(name+'.asm')).write_text(asm)
        (out.parent/(name+'.o')).write_bytes(obj)
        (out.parent/(name+'.bin')).write_bytes(raw)
        report['shaders'][name]={'vgpr':nv,'sgpr':ns,'user_sgprs':nuser,'lds_bytes':lds,'scratch_bytes':scratch,'rsrc1':r1,'rsrc2':r2,'threads':[nx,ny,1],'occupancy_compiler_waves_per_simd':stat(asm,'Occupancy'),'flags':args[1:],'raw_bytes':len(raw),'padded_bytes':4*len(padded),'source_sha256':hashlib.sha256(ll.read_bytes()).hexdigest(),'raw_sha256':hashlib.sha256(raw).hexdigest(),'elf_sha256':hashlib.sha256(obj).hexdigest(),'entry_offset':0,'relocations':0,'padded_sha256':hashlib.sha256(struct.pack('<%dI'%len(padded),*padded)).hexdigest()}
        print(f'{name}: vgpr={nv} sgpr={ns} user={nuser} lds={lds} scratch={scratch} raw={len(raw)} padded={4*len(padded)} rsrc1=0x{r1:08x} rsrc2=0x{r2:08x}')
    tmp=out.with_name(out.name+'.tmp');tmp.write_text('\n'.join(lines)+'\n');tmp.replace(out)
    (out.parent/'shader-resources.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('llc');p.add_argument('shaders',type=pathlib.Path);p.add_argument('out',type=pathlib.Path)
    a=p.parse_args()
    try:
        build(a.llc,a.shaders,a.out)
    except subprocess.CalledProcessError as e:
        detail=e.stderr.decode(errors='replace') if isinstance(e.stderr,bytes) else e.stderr
        raise SystemExit(str(e)+'\n'+(detail or ''))
    except ValueError as e:
        raise SystemExit(str(e))
