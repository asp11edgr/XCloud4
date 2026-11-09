#!/usr/bin/env python3
"""Run the real 720p -> 1080p LLVM IR conversion on a host ABI and validate raw shader ELF.

Only entry calling convention, raw-buffer I/O names and target metadata are
changed for the host. Every arithmetic/filter/load/store offset remains from
the shipped .ll source. This does not execute a PS4 GPU or prove PM4 behavior.
"""
import argparse,hashlib,importlib.util,json,os,pathlib,re,shlex,subprocess,tempfile

root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=pathlib.Path)
p.add_argument('--fixtures',type=pathlib.Path,default=os.environ.get('X4_GPU_REAL_720_FIXTURE_MANIFEST'))
p.add_argument('--artifacts',type=pathlib.Path,default=os.environ.get('X4_GPU_SHADER_ARTIFACT_DIR'))
a=p.parse_args()
out=a.output or pathlib.Path(tempfile.mkdtemp(prefix='x4-gpu-convert-720-'))
out.mkdir(parents=True,exist_ok=True)
if not a.fixtures or not a.artifacts:
    raise SystemExit('UNAVAILABLE GPU acceptance requires X4_GPU_REAL_720_FIXTURE_MANIFEST and X4_GPU_SHADER_ARTIFACT_DIR')
manifest=json.loads(a.fixtures.read_text())
report=json.loads((a.artifacts/'shader-resources.json').read_text())
# Immutable TAREA23b R2 source hashes and padded machine-code hashes.  New
# generation may add 720p arrays but must not change these validated kernels.
immutable1080={
    'original':('3a758efbf2db99da6a974456c43e5939168ab0e845fd777e5bb7d59dc659dda0','855255f88edc130a22927b60594f330392940956bf6082478ed2beb741ca0521'),
    'smooth0':('5c308d9b6f9e2430d602161952e7237c17fa986068347d5103ba8c5d2e1d667f','25b0949598523fd810111bc28f50ccbe418c010839774458d8774f3295c09c64'),
    'smooth1':('f616d54f27cafced86d558f8182a11721ba5409c5ed79e92f93fa1a5111feff8','9c9ce84ee7d56ef102f6b19335e26c69027da0f1ded01395100461e9c2fec554'),
    'smooth2':('3aeed0cff4acbf66a96207b69d735484fbeda623c5d67e42114b270082aaa65c','60f4cd7ec7a87b4567d0afa8efe7c5d4c9b91fdbc05fd483d3a53b060c9dc6cc')}
for name,(source_sha,padded_sha) in immutable1080.items():
    assert hashlib.sha256((root/'src/video/gpu'/(name+'.ll')).read_bytes()).hexdigest()==source_sha
    assert report['shaders'][name]['source_sha256']==source_sha
    assert report['shaders'][name]['padded_sha256']==padded_sha
sources={x['sha256']:x for x in manifest['sources']}
fixtures=[]
for frame in manifest['frames']:
    path=a.fixtures.parent/frame['path']
    assert path.parent==a.fixtures.parent and path.is_file()
    assert path.stat().st_size==frame['bytes']==1382400
    assert hashlib.sha256(path.read_bytes()).hexdigest()==frame['sha256']
    assert frame['pitch_y']==1280 and frame['storage_height']==720 and frame['visible_height']==720
    info=sources[frame['source_sha256']]['ffprobe']['streams'][0]
    assert info['color_space'] in ('bt709','smpte170m') and info['color_range'] in ('pc','tv')
    fixtures.extend([str(path.resolve()),'1' if info['color_space']=='bt709' else '6','1' if info['color_range']=='pc' else '0'])
assert len(manifest['frames'])>=4
assert {(sources[f['source_sha256']]['ffprobe']['streams'][0]['color_space'],
         sources[f['source_sha256']]['ffprobe']['streams'][0]['color_range'])
        for f in manifest['frames']} >= {('bt709','tv'),('bt709','pc'),('smpte170m','tv'),('smpte170m','pc')}
assert all(sources[f['source_sha256']]['ffprobe']['streams'][0]['codec_name']=='h264' for f in manifest['frames'])
spec=importlib.util.spec_from_file_location('x4_gpu_generator',root/'scripts/gen_gpu_convert_shaders.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
checks=0
def rejected(call):
    global checks
    try:call()
    except ValueError:checks+=1;return
    raise AssertionError('malformed shader ABI/ELF was accepted')
for name in ('original720','smooth720_0','smooth720_1','smooth720_2'):
    source=(root/'src/video/gpu'/(name+'.ll')).read_text()
    assert gen.abi(name,source)==(16,1,1,8,8)
    assert hashlib.sha256(source.encode()).hexdigest()==report['shaders'][name]['source_sha256']
    metadata=report['shaders'][name]
    assert metadata['relocations']==metadata['scratch_bytes']==metadata['lds_bytes']==0
    r1,r2=gen.resources(metadata['vgpr'],metadata['sgpr'],16,0,0,1,1)
    assert r1==metadata['rsrc1'] and r2==metadata['rsrc2']
    assert metadata['threads']==[8,8,1] and metadata['entry_offset']==0
    obj=(a.artifacts/(name+'.o')).read_bytes()
    words=gen.text_words(obj)
    import struct
    raw=struct.pack('<%dI'%len(words),*words)
    assert raw==(a.artifacts/(name+'.bin')).read_bytes()
    assert hashlib.sha256(obj).hexdigest()==metadata['elf_sha256']
    rejected(lambda source=source:gen.abi(name,source.replace('; user_sgprs=16','; user_sgprs=12')))
    rejected(lambda source=source:gen.abi(name,source.replace('<4 x i32> inreg %params','<4 x i32> %params')))
    for key,expected,bad in (('threads_x',8,16),('threads_y',8,4),('tgid_y',1,0),('local_y',1,0)):
        rejected(lambda source=source,key=key,expected=expected,bad=bad:gen.abi(name,source.replace('; '+key+'='+str(expected),'; '+key+'='+str(bad))))
    rejected(lambda source=source:gen.abi(name,source.replace('target triple = \"amdgcn--\"','target triple = \"amdgcn-amd-amdhsa\"')))
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
for name in ('original720','smooth720_0','smooth720_1','smooth720_2'):
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
    command+=['tests/gpu_convert_720_ir_test.c','src/video/video_pipeline.c',*objects,'-o',str(binary)]
    subprocess.run(command,cwd=root,check=True)
    logfile=out/(binary.name+'.log')
    with logfile.open('w') as stream:subprocess.run([str(binary),*fixtures],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
    text=logfile.read_text();assert 'PASS actual LLVM 720p shader arithmetic' in text
    print(text.splitlines()[-1]+' CPU='+('scalar' if scalar else 'SSE2'),flush=True)
    results.append({'cpu':'scalar' if scalar else 'SSE2','passed':True,'log':logfile.name,
        'log_sha256':hashlib.sha256(logfile.read_bytes()).hexdigest()})
(out/'gpu-720-equivalence-result.json').write_text(json.dumps({'passed':True,'scope':'actual LLVM 720p -> 1080p arithmetic on x86 raw-buffer ABI; bit-exact CPU oracle, not GPU execution',
    'fixtures_manifest_sha256':hashlib.sha256(a.fixtures.read_bytes()).hexdigest(),'real_fixture_frames':len(manifest['frames']),
    'immutable_1080_hash_checks':len(immutable1080)*3,'shader_rejection_checks':checks,'translations':translations,'results':results},indent=2)+'\n')
