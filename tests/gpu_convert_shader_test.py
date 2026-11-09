#!/usr/bin/env python3
"""Run the real LLVM IR conversion on a host ABI and validate raw shader ELF.

Only entry calling convention, raw-buffer I/O names and target metadata are
changed for the host. Every arithmetic/filter/load/store offset remains from
the shipped .ll source. This does not execute a PS4 GPU or prove PM4 behavior.
"""
import argparse,hashlib,importlib.util,json,os,pathlib,re,shlex,subprocess,tempfile

root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=pathlib.Path)
p.add_argument('--fixtures',type=pathlib.Path,default=os.environ.get('X4_GPU_REAL_FIXTURE_MANIFEST'))
p.add_argument('--artifacts',type=pathlib.Path,default=os.environ.get('X4_GPU_SHADER_ARTIFACT_DIR'))
a=p.parse_args()
out=a.output or pathlib.Path(tempfile.mkdtemp(prefix='x4-gpu-convert-'))
out.mkdir(parents=True,exist_ok=True)
if not a.fixtures or not a.artifacts:
    raise SystemExit('UNAVAILABLE GPU acceptance requires X4_GPU_REAL_FIXTURE_MANIFEST and X4_GPU_SHADER_ARTIFACT_DIR')
manifest=json.loads(a.fixtures.read_text())
report=json.loads((a.artifacts/'shader-resources.json').read_text())
sources={x['sha256']:x for x in manifest['sources']}
fixtures=[]
for frame in manifest['frames']:
    path=a.fixtures.parent/frame['path']
    assert path.parent==a.fixtures.parent and path.is_file()
    assert path.stat().st_size==frame['bytes']==3133440
    assert hashlib.sha256(path.read_bytes()).hexdigest()==frame['sha256']
    assert frame['pitch_y']==1920 and frame['storage_height']==1088 and frame['visible_height']==1080
    info=sources[frame['source_sha256']]['ffprobe']['streams'][0]
    assert info['color_space'] in ('bt709','smpte170m') and info['color_range'] in ('pc','tv')
    fixtures.extend([str(path.resolve()),'1' if info['color_space']=='bt709' else '6','1' if info['color_range']=='pc' else '0'])
assert len(manifest['frames'])>=4
spec=importlib.util.spec_from_file_location('x4_gpu_generator',root/'scripts/gen_gpu_convert_shaders.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
checks=0
def rejected(call):
    global checks
    try:call()
    except ValueError:checks+=1;return
    raise AssertionError('malformed shader ABI/ELF was accepted')
for name in ('original','smooth0','smooth1','smooth2'):
    source=(root/'src/video/gpu'/(name+'.ll')).read_text()
    assert gen.abi(name,source)==(16,1,1,8,8)
    assert hashlib.sha256(source.encode()).hexdigest()==report['shaders'][name]['source_sha256']
    metadata=report['shaders'][name]
    assert metadata['relocations']==metadata['scratch_bytes']==metadata['lds_bytes']==0
    obj=(a.artifacts/(name+'.o')).read_bytes()
    words=gen.text_words(obj)
    import struct
    raw=struct.pack('<%dI'%len(words),*words)
    assert raw==(a.artifacts/(name+'.bin')).read_bytes()
    assert hashlib.sha256(obj).hexdigest()==metadata['elf_sha256']
    rejected(lambda source=source:gen.abi(name,source.replace('; user_sgprs=16','; user_sgprs=12')))
    rejected(lambda source=source:gen.abi(name,source.replace('<4 x i32> inreg %params','<4 x i32> %params')))
    rejected(lambda obj=obj:gen.text_words(b'bad!'+obj[4:]))
    broken=bytearray(obj);struct.pack_into('<Q',broken,24,4);rejected(lambda broken=broken:gen.text_words(broken))
    # Turn the nonempty .text into RELA: even an otherwise valid ELF must fail.
    broken=bytearray(obj);shoff=struct.unpack_from('<Q',obj,40)[0]
    count,strings=struct.unpack_from('<HH',obj,60)
    stringsec=struct.unpack_from('<IIQQQQIIQQ',obj,shoff+64*strings)
    names=obj[stringsec[4]:stringsec[4]+stringsec[5]]
    for index in range(1,count):
        off=shoff+64*index;sec=struct.unpack_from('<IIQQQQIIQQ',obj,off)
        if names[sec[0]:].split(b'\0',1)[0]==b'.text':struct.pack_into('<I',broken,off+4,4);break
    rejected(lambda broken=broken:gen.text_words(broken))
for params in ((0,22,16,0,0,1,1),(257,22,16,0,0,1,1),(15,105,16,0,0,1,1),
               (15,22,12,0,0,1,1),(15,22,16,0,1,1,1),(15,22,16,32769,0,1,1)):
    rejected(lambda params=params:gen.resources(*params))
print('PASS GPU shader validation and rejection checks:',checks,flush=True)
cc=shlex.split(os.environ.get('X4_GPU_HOST_CC',os.environ.get('CC','clang')))
flags=shlex.split(os.environ.get('X4_TEST_CFLAGS',''))
host_target=subprocess.check_output(cc+['-dumpmachine'],text=True).strip()
assert re.fullmatch(r'x86_64-[A-Za-z0-9_.-]+',host_target),host_target
base=['-O2','-g','-fno-omit-frame-pointer']
objects=[]
translations=[]
for name in ('original','smooth0','smooth1','smooth2'):
    original=root/'src/video/gpu'/(name+'.ll');source=original.read_text()
    host=source.replace('target triple = "amdgcn--"','target triple = "'+host_target+'"')
    host=host.replace('define amdgpu_cs void @main(','define void @x4_gpu_test_'+name+'(')
    host=host.replace(' inreg ',' ').replace(' immarg','')
    host=host.replace('@llvm.amdgcn.raw.buffer.load.i32','@x4_gpu_test_raw_load')
    host=host.replace('@llvm.amdgcn.raw.buffer.store.i32','@x4_gpu_test_raw_store')
    host=host.replace(') #0 {',') {')
    host=host.replace('attributes #0 = { "amdgpu-flat-work-group-size"="64,64" }','')
    translated=out/(name+'-host.ll');translated.write_text(host)
    obj=out/(name+'-host.o')
    irflags=[x for x in flags if x.startswith(('-fsanitize=','-fno-','--resource-dir=','-resource-dir='))]
    subprocess.run(cc+base+irflags+['-x','ir','-c',str(translated),'-o',str(obj)],check=True)
    objects.append(str(obj));translations.append({'shader':name,'source_sha256':hashlib.sha256(original.read_bytes()).hexdigest(),
        'host_ir_sha256':hashlib.sha256(translated.read_bytes()).hexdigest()})
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
results=[]
for scalar in (0,1):
    binary=out/('equivalence-scalar' if scalar else 'equivalence-sse2')
    command=cc+['-std=gnu2x']+base+flags+['-Wall','-Wextra','-Werror','-DX4_PUBLIC=0','-DX4_STREAM_PROFILE=1','-DX4_VIDEO_PROBE=0']
    if scalar:command+=['-DX4_PIPELINE_SCALAR=1']
    command+=['tests/gpu_convert_ir_test.c','src/video/video_pipeline.c',*objects,'-o',str(binary)]
    subprocess.run(command,cwd=root,check=True)
    logfile=out/(binary.name+'.log')
    with logfile.open('w') as stream:subprocess.run([str(binary),*fixtures],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
    text=logfile.read_text();assert 'PASS actual LLVM shader arithmetic' in text
    print(text.splitlines()[-1]+' CPU='+('scalar' if scalar else 'SSE2'),flush=True)
    results.append({'cpu':'scalar' if scalar else 'SSE2','passed':True,'log':logfile.name,
        'log_sha256':hashlib.sha256(logfile.read_bytes()).hexdigest()})
(out/'gpu-equivalence-result.json').write_text(json.dumps({'passed':True,'scope':'actual LLVM arithmetic on x86 raw-buffer ABI; not GPU hardware execution',
    'fixtures_manifest_sha256':hashlib.sha256(a.fixtures.read_bytes()).hexdigest(),'real_fixture_frames':len(manifest['frames']),
    'shader_rejection_checks':checks,'translations':translations,'results':results},indent=2)+'\n')
