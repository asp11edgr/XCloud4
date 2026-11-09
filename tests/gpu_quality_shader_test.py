#!/usr/bin/env python3
"""Validate quality ELF/ABI and run the delivered LLVM arithmetic on x86.

Only the entry ABI, raw I/O intrinsic names and target metadata are adapted.
The CPU oracle is the independently authored compact-row reference. Host
equivalence is not a claim of PS4 execution, cache coherence or performance.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shlex
import struct
import subprocess
import tempfile

root=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=pathlib.Path)
parser.add_argument('--fixtures',type=pathlib.Path,default=os.environ.get('X4_GPU_REAL_FIXTURE_MANIFEST'))
parser.add_argument('--artifacts',type=pathlib.Path,default=os.environ.get('X4_GPU_SHADER_ARTIFACT_DIR'))
args=parser.parse_args()
out=args.output or pathlib.Path(tempfile.mkdtemp(prefix='x4-gpu-quality-'))
out.mkdir(parents=True,exist_ok=True)
if not args.fixtures or not args.artifacts:
    raise SystemExit('UNAVAILABLE quality acceptance requires fixture manifest and actual llc shader artifacts')
manifest=json.loads(args.fixtures.read_text())
report=json.loads((args.artifacts/'shader-resources.json').read_text())
sources={item['sha256']:item for item in manifest['sources']}
fixtures=[]
for frame in manifest['frames']:
    path=args.fixtures.parent/frame['path']
    assert path.parent==args.fixtures.parent and path.is_file()
    assert path.stat().st_size==frame['bytes']==3133440
    assert hashlib.sha256(path.read_bytes()).hexdigest()==frame['sha256']
    assert frame['pitch_y']==1920 and frame['storage_height']==1088 and frame['visible_height']==1080
    info=sources[frame['source_sha256']]['ffprobe']['streams'][0]
    assert info['color_space'] in ('bt709','smpte170m') and info['color_range'] in ('pc','tv')
    fixtures.extend([str(path.resolve()),'1' if info['color_space']=='bt709' else '6','1' if info['color_range']=='pc' else '0'])
assert len(manifest['frames'])>=3
spec=importlib.util.spec_from_file_location('x4_shader_generator',root/'scripts/gen_gpu_convert_shaders.py')
gen=importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
assert len(gen.immutable_baseline(root/'src/video/gpu'))==8
checks=0


def rejected(call):
    global checks
    try:
        call()
    except ValueError:
        checks+=1
        return
    raise AssertionError('malformed shader ABI/ELF/strict arithmetic was accepted')


for name in gen.SHADERS_QUALITY:
    source=(root/'src/video/gpu'/(name+'.ll')).read_text()
    metadata=report['shaders'][name]
    assert gen.abi(name,source)==(16,1,1,8,8)
    assert hashlib.sha256((root/'src/video/gpu'/(name+'.ll')).read_bytes()).hexdigest()==metadata['source_sha256']
    assert metadata['relocations']==metadata['scratch_bytes']==metadata['lds_bytes']==0
    assert metadata['threads']==[8,8,1] and metadata['entry_offset']==0
    assert metadata['occupancy_compiler_waves_per_simd']>=1
    assert metadata['flags']==['-mcpu=gfx700','-O2','-fp-contract=off']
    asm=(args.artifacts/(name+'.asm')).read_text()
    gen.quality_assembly(asm)
    obj=(args.artifacts/(name+'.o')).read_bytes()
    words=gen.text_words(obj)
    raw=struct.pack('<%dI'%len(words),*words)
    assert raw==(args.artifacts/(name+'.bin')).read_bytes()
    assert hashlib.sha256(obj).hexdigest()==metadata['elf_sha256']
    assert hashlib.sha256(raw).hexdigest()==metadata['raw_sha256']
    padded=words+[0xBF810000]*((-len(words))%64+16)
    assert hashlib.sha256(struct.pack('<%dI'%len(padded),*padded)).hexdigest()==metadata['padded_sha256']
    header=(root/'src/video/gpu/gpu_shaders.h').read_text()
    match=re.search(r'static const uint32_t x4_gpu_'+name+r'_code\[\d+\] = \{(.*?)\};',header,re.S)
    assert match
    assert [int(word,16) for word in re.findall(r'0x([0-9A-Fa-f]{8})u',match[1])]==padded
    rejected(lambda source=source,name=name:gen.abi(name,source.replace('; user_sgprs=16','; user_sgprs=12')))
    rejected(lambda source=source,name=name:gen.abi(name,source.replace('<4 x i32> inreg %params','<4 x i32> %params')))
    rejected(lambda obj=obj:gen.text_words(b'bad!'+obj[4:]))
    broken=bytearray(obj)
    struct.pack_into('<Q',broken,24,4)
    rejected(lambda broken=broken:gen.text_words(broken))
    broken=bytearray(obj)
    shoff=struct.unpack_from('<Q',obj,40)[0]
    count,strings=struct.unpack_from('<HH',obj,60)
    stringsec=struct.unpack_from('<IIQQQQIIQQ',obj,shoff+64*strings)
    names=obj[stringsec[4]:stringsec[4]+stringsec[5]]
    for index in range(1,count):
        offset=shoff+64*index
        section=struct.unpack_from('<IIQQQQIIQQ',obj,offset)
        if names[section[0]:].split(b'\0',1)[0]==b'.text':
            struct.pack_into('<I',broken,offset+4,4)
            break
    rejected(lambda broken=broken:gen.text_words(broken))
for params in ((0,22,16,0,0,1,1),(257,22,16,0,0,1,1),(15,105,16,0,0,1,1),
               (15,22,12,0,0,1,1),(15,22,16,0,1,1,1),(15,22,16,32769,0,1,1)):
    rejected(lambda params=params:gen.resources(*params))
for instruction in ('v_fma_f32 v0,v1,v2,v3','v_madak_f32 v0,v1,v2,0','v_mac_f32 v0,v1,v2',
                    'v_fma_f32_e64 v0,v1,v2,v3','v_mac_f32_e32 v0,v1,v2',
                    'v_rcp_f32 v0,v1','v_rsq_f32 v0,v1','v_sqrt_f32 v0,v1','v_div_fmas_f32 v0,v1,v2,v3'):
    rejected(lambda instruction=instruction:gen.quality_assembly(instruction))
gen.quality_assembly('v_mad_u32_u24 v0,v1,v2,v3\nv_add_f32 v0,v1,v2')
gen.quality_ir('attributes #0 = { "fp-contract"="off" "unsafe-fp-math"="false" }')
for unsafe in ('%x = fmul contract float %a, %b','%x = fmul fast float %a, %b',
               'attributes #0 = { "fp-contract"="fast" }','attributes #0 = { "unsafe-fp-math"="true" }'):
    rejected(lambda unsafe=unsafe:gen.quality_ir(unsafe))
with tempfile.TemporaryDirectory(prefix='x4-legacy-protection-') as temporary:
    clone=pathlib.Path(temporary)
    header_path=clone/'gpu_shaders.h'
    header=(root/'src/video/gpu/gpu_shaders.h').read_text()
    for name in gen.SHADERS_LEGACY:
        (clone/(name+'.ll')).write_bytes((root/'src/video/gpu'/(name+'.ll')).read_bytes())
    header_path.write_text(header)
    assert len(gen.immutable_baseline(clone))==8
    (clone/'smooth720_2.ll').write_bytes((clone/'smooth720_2.ll').read_bytes()+b'\n; forbidden baseline edit\n')
    rejected(lambda:gen.immutable_baseline(clone))
    (clone/'smooth720_2.ll').write_bytes((root/'src/video/gpu/smooth720_2.ll').read_bytes())
    header_path.write_text(header.replace('#define X4_GPU_SMOOTH720_2_VGPR 38u','#define X4_GPU_SMOOTH720_2_VGPR 37u'))
    rejected(lambda:gen.immutable_baseline(clone))
    header_path.write_text(header.replace('0xBF810000u','0xBF810001u',1))
    rejected(lambda:gen.immutable_baseline(clone))
print('PASS quality ABI/ELF/strict-ISA rejection checks:',checks,'immutable R3 anchors: 24',flush=True)
noise_header=root/'src/video/gpu/gpu_quality_noise.h'
noise_regenerated=out/'gpu_quality_noise.h'
noise_report=out/'noise-regeneration.json'
subprocess.run([os.sys.executable,'-B',str(root/'scripts/gen_gpu_quality_noise.py'),
                '--header',str(noise_regenerated),'--report',str(noise_report)],check=True)
assert noise_regenerated.read_bytes()==noise_header.read_bytes()
noise=json.loads(noise_report.read_text())
assert noise['rank_bytes_sha256']=='67e5cbcef5cdb8f56d278845c0af83bfc0e1f86f853c0b0eaa5c68c58b33d82b'
assert noise['unique_ranks']==4096 and noise['minimum']==0 and noise['maximum']==4095
print('PASS deterministic noise regeneration, 4096 unique ranks and canonical little-endian rank hash',flush=True)
cc=shlex.split(os.environ.get('X4_GPU_HOST_CC',os.environ.get('CC','clang')))
flags=shlex.split(os.environ.get('X4_TEST_CFLAGS',''))
host_target=subprocess.check_output(cc+['-dumpmachine'],text=True).strip()
assert re.fullmatch(r'x86_64-[A-Za-z0-9_.-]+',host_target),host_target
base=['-O2','-g','-fno-omit-frame-pointer','-ffp-contract=off']
objects=[]
translations=[]
for name in gen.SHADERS_QUALITY:
    original=root/'src/video/gpu'/(name+'.ll')
    source=original.read_text()
    host=source.replace('target triple = "amdgcn--"','target triple = "'+host_target+'"')
    host=host.replace('define amdgpu_cs void @main(','define void @x4_gpu_test_'+name+'(')
    host=host.replace(' inreg ',' ').replace(' immarg','')
    host=host.replace('@llvm.amdgcn.raw.buffer.load.i32','@x4_gpu_test_raw_load')
    host=host.replace('@llvm.amdgcn.raw.buffer.store.i32','@x4_gpu_test_raw_store')
    host=re.sub(r'"(?:amdgpu[^"=]*|target-cpu|target-features)"(?:="[^"]*")?','',host)
    translated=out/(name+'-host.ll')
    translated.write_text(host)
    obj=out/(name+'-host.o')
    irflags=[flag for flag in flags if flag.startswith(('-fsanitize=','-fno-','--resource-dir=','-resource-dir='))]
    subprocess.run(cc+base+irflags+['-x','ir','-c',str(translated),'-o',str(obj)],check=True)
    objects.append(str(obj))
    translations.append({'shader':name,'source_sha256':hashlib.sha256(original.read_bytes()).hexdigest(),
                         'host_ir_sha256':hashlib.sha256(translated.read_bytes()).hexdigest()})
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
results=[]
for scalar in (0,1):
    binary=out/('quality-equivalence-scalar' if scalar else 'quality-equivalence-sse2')
    command=cc+['-std=gnu2x']+base+flags+['-Wall','-Wextra','-Werror','-DX4_PUBLIC=0','-DX4_STREAM_PROFILE=1','-DX4_VIDEO_PROBE=0']
    if scalar:
        command+=['-DX4_PIPELINE_SCALAR=1','-fno-vectorize','-fno-slp-vectorize']
    else:
        command+=['-msse2']
    command+=['tests/gpu_quality_ir_test.c','src/video/gpu_quality_ref.c','src/video/video_pipeline.c',*objects,'-o',str(binary)]
    subprocess.run(command,cwd=root,check=True)
    logfile=out/(binary.name+'.log')
    with logfile.open('w') as stream:
        subprocess.run([str(binary),*fixtures],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
    output=logfile.read_text()
    summary=output.splitlines()[-1]
    match=re.fullmatch(r'PASS actual LLVM quality arithmetic against compact CPU reference: cases=(\d+) pixels=(\d+) maximum=(\d+) different=(\d+) outside=(\d+)',summary)
    assert match and int(match[3])<=1 and int(match[5])==0
    print(summary+' CPU='+('scalar' if scalar else 'SSE2'),flush=True)
    results.append({'cpu':'scalar' if scalar else 'SSE2','passed':True,'cases':int(match[1]),'pixels':int(match[2]),
                    'maximum':int(match[3]),'different_pixels':int(match[4]),'outside':int(match[5]),'log':logfile.name,
                    'log_sha256':hashlib.sha256(logfile.read_bytes()).hexdigest()})
(out/'gpu-quality-equivalence-result.json').write_text(json.dumps({'passed':True,
    'scope':'actual delivered LLVM arithmetic on x86 raw-buffer ABI; independent compact-row CPU oracle; no GPU execution',
    'fixtures_manifest_sha256':hashlib.sha256(args.fixtures.read_bytes()).hexdigest(),'decoded_fixture_frames':len(manifest['frames']),
    'immutable_r3_anchors':24,'shader_rejection_checks':checks,'noise_rank_sha256':noise['rank_bytes_sha256'],
    'noise_regeneration_identical':True,'translations':translations,'results':results},indent=2)+'\n')
