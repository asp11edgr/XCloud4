#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T27 extensions over unchanged T26/streaming native audit implementations.

Same ELF + map + disassembly + .su + source hashes remain mandatory. No native
ELF is executed. New roots expand coverage; no old root, allowance or check is
removed. This is a conservative stack model, never a PS4 high-water result.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import audit_probe_stack as stack
import audit_probe_posix_elf as posix
import audit_probe_t26_posix_elf as t26_posix
import prepare_probe_t26_stack_policy as t26
import prepare_stream_stack_policy as stream
import audit_stream_posix_elf as stream_posix

GPU_UNITS={'gpu_convert.c','gpu_quality_ref.c'}
T27_UNITS={'probe_t27_'+name+'.c' for name in ('controller','plan','state','store','result','view')}
GPU_NATIVE=('sceGnmAreSubmitsAllowed','sceGnmSubmitCommandBuffers','sceGnmSubmitDone',
    'sceKernelAllocateDirectMemory','sceKernelMapDirectMemory','sceKernelMunmap','sceKernelReleaseDirectMemory')
WORKER='stream_pipeline_verification_worker'
T27_REQUIRED=('x4_t27_controller_run','t27_bank_worker','x4_t27_view_draw',
    'x4_t27_native_result_encode','x4_t27_native_result_write','x4_t27_native_result_read')
T27_BINDINGS=tuple(('t27_bank_worker','probe_t27_controller.c',member,target) for member,target in (
    ('now_us','t27_clock_us'),('sleep_us','t27_sleep_us'),('budget_expired','t27_expired'),
    ('before','t27_before_native'),('display_open','t27_display_open'),('display_close','t27_display_close')))

def evidence(path,output,detail,kind=None):
    path=Path(path)
    result={'artifact':Path(os.path.relpath(path.resolve(),Path(output).parent.resolve())).as_posix(),
        'sha256':stack.sha256(path.read_bytes()),'detail':detail}
    if kind:result['kind']=kind
    return result

def new_roots(symbols,functions,probe):
    required=(WORKER,)+ (T27_REQUIRED if probe else ())
    roots={t26.linked_name(symbols,functions,name) for name in required}
    # Every linked new bank helper is independently audited. A compiler may
    # inline some helpers; the owning root/.su frame remains in that case.
    for key,fn in functions.items():
        if probe and fn['name'].startswith(('t27_','x4_t27_')):roots.add(key)
    return sorted(roots)

def binding_proofs(symbols,functions,source_root,output):
    proofs=[];callbacks=[]
    for owner,unit,member,target in T27_BINDINGS:
        path=source_root/'src/video'/unit
        table=t26.source_functions(path.read_text(encoding='utf8'))
        if owner not in table or not re.search(r'\.'+member+r'\s*=\s*'+target+r'\b',table[owner]['original']):
            raise ValueError('T27 callback assignment changed: '+owner+'.'+member)
        own=t26.linked_name(symbols,functions,owner);matches=[]
        for ins in functions[own]['instructions']:
            m=re.search(r'(?:#\s*)?(?:0x)?([0-9a-fA-F]+)\s+<'+target+r'>',ins['operands'])
            if m and not ins['mnemonic'].startswith(('call','j')):
                matches.append((ins,t26.linked_name(symbols,functions,target,int(m[1],16))))
        if not matches:raise ValueError('Missing same-ELF T27 callback address: '+member+' -> '+target)
        ins,linked=matches[0]
        proof=evidence(path,output,'Exact T27 source callback member and same-ELF address-taking instruction; unresolved dispatch still carries the unchanged 16 KiB charge.')
        proofs.append(proof);callbacks.append({'constructor':own,'member':member,'target':linked,
            'address_taking_site':ins['address'],'instruction':ins['text'],'source_line':table[owner]['line'],'source_evidence':proof})
    return callbacks,proofs

def extend_policy(policy,elf,su_root,output,source_root,symbols,functions,probe):
    result=copy.deepcopy(policy)
    roots=new_roots(symbols,functions,probe)
    result['additional_roots']=sorted(set(result['additional_roots'])|set(roots))
    config=evidence(source_root/'src/video/probe_stack.h',output,
        'T27 and verification workers use the existing checked explicit 256 KiB thread wrapper; console stack and high-water remain unmeasured.','configured_attribute')
    entries=[('x4-gpu-verify',WORKER)]+([('x4-t27-bank','t27_bank_worker')] if probe else [])
    for name,entry in entries:
        result['threads'].append({'name':name,'entry':t26.linked_name(symbols,functions,entry),
            'wrapper_entry':'x4_probe_thread_trampoline','stack_bytes':256*1024,'evidence':config,
            'limit_status':'explicit native 256 KiB request; runtime high-water is separate evidence'})
    own=set(result['own_functions']);new_sources=[];seen_units=set();units=GPU_UNITS|(T27_UNITS if probe else set())
    for path in sorted(Path(su_root).rglob('*.su')):
        records=stack.parse_stack_usage(path.read_text(encoding='utf8'),str(path))
        for name,rows in records.items():
            matching={Path(row['location'].replace('\\','/').split(':',1)[0]).name for row in rows}&units
            if matching:
                seen_units|=matching;own.add(name)
                own.update(key for key,fn in functions.items() if fn['name']==name)
    if units-seen_units:raise ValueError('Missing same-build .su for new owned units: '+', '.join(sorted(units-seen_units)))
    result['own_functions']=sorted(own)
    for unit in sorted(units):
        path=source_root/'src/video'/unit
        if not path.is_file():raise ValueError('Missing new owned source: '+unit)
        new_sources.append(evidence(path,output,'New T27/GPU source ownership is bound to this same-build stack model; no function receives a relaxed frame limit.'))
    result['source_evidence']+=new_sources+[config]
    result['required_native_imports']=sorted(set(result['required_native_imports'])|set(stream.STREAM_NATIVE_WAIT_IMPORTS)|set(GPU_NATIVE))
    if probe:
        callbacks,proofs=binding_proofs(symbols,functions,source_root,output)
        result['source_evidence']+=proofs;result['t27_callback_bindings']=callbacks
        result['additional_roots']=sorted(set(result['additional_roots'])|{row['target'] for row in callbacks})
    result['t27_scope']='T26/stream roots retained; all new T27 helpers and asynchronous verifier included. Fixed 8192 B frames, 256 KiB workers, 50 percent capacity, 16 KiB unresolved native/indirect allowance.'
    strict_policy(result)
    return result

def strict_policy(policy):
    if policy.get('max_fraction')!=.5 or policy.get('individual_frame_limit_bytes')!=8192 or policy.get('opaque_allowance_bytes')!=16384:
        raise ValueError('T27 stack constants must remain exactly 50 percent / 8192 / 16384')
    for thread in policy.get('threads',[]):
        if thread.get('name')!='main' and thread.get('stack_bytes')!=256*1024:
            raise ValueError('T27 native worker stack must remain exactly 256 KiB')
    if not any(row.get('entry')==WORKER for row in policy.get('threads',[])):
        raise ValueError('Missing verifier worker stack root')

def posix_audit(elf,output,source_root,dump,probe):
    body=elf.read_bytes();symbols=stack.elf_symbols(body);functions=stack.parse_disassembly(dump)
    roots=new_roots(symbols,functions,probe)
    if probe:
        # Re-run the unchanged strict gate with every old root/callback AND
        # the new bank/verifier roots. Never treat a prior PASS as sufficient.
        previous=t26_posix.audit(symbols,functions,source_root,body)
        callbacks,proofs=binding_proofs(symbols,functions,source_root,output)
        old_roots=[t26.linked_name(symbols,functions,name) for name in t26_posix.ROOT_NAMES]
        indirect=[t26.linked_name(symbols,functions,name) for name in t26_posix.FILE_ROOTS]
        indirect += [row['target'] for row in t26.callback_bindings(symbols,functions,source_root)]
        indirect += [row['target'] for row in callbacks]
        result=posix.audit(symbols,t26_posix.posix_blocks(symbols,functions),roots=tuple(old_roots+roots),
            required_imports=('fstat','sceKernelFsync'),indirect_roots=tuple(sorted(set(indirect))),all_imports_in_scope=True)
        result['passed'] &= previous['passed']
        result['t26_dynamic_cpu_proof']=previous['t26_dynamic_cpu_proof']
        result['forbidden_static_cpu_routes']=previous['forbidden_static_cpu_routes']
        result['t27_callback_bindings']=callbacks;result['t27_source_evidence']=proofs
    else:
        result=posix.audit(symbols,t26_posix.posix_blocks(symbols,functions),
            roots=tuple(stream_posix.ROOTS)+tuple(roots),required_imports=('fstat','sceKernelFsync')+stream.STREAM_NATIVE_WAIT_IMPORTS,
            indirect_roots=stream_posix.INDIRECT,all_imports_in_scope=False)
    result.update(schema='X4_T27_POSIX_AUDIT1',elf_sha256=stack.sha256(body),elf_bytes=len(body),
        disassembly_sha256=stack.sha256(dump.encode()),scope='Expanded roots over unchanged strict POSIX/ENOSYS gate; no ELF execution.')
    return result

def negative_tests():
    good={'max_fraction':.5,'individual_frame_limit_bytes':8192,'opaque_allowance_bytes':16384,
        'threads':[{'name':'x4-gpu-verify','entry':WORKER,'stack_bytes':256*1024}]}
    strict_policy(good);rejected=0
    for key,value in [('max_fraction',.51),('max_fraction',.49),('individual_frame_limit_bytes',8193),('opaque_allowance_bytes',16383)]:
        bad=copy.deepcopy(good);bad[key]=value
        try:strict_policy(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('relaxed policy accepted: '+key)
    for bad in (dict(good,threads=[]),dict(good,threads=[dict(good['threads'][0],stack_bytes=512*1024)])):
        try:strict_policy(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('missing/oversized worker accepted')
    # Exercise the unchanged auditor negatives too: dynamic frame, 8193 B
    # frame, recursion, known ENOSYS and stale evidence are covered there.
    for path in ('tests/probe_stack_audit_test.py','tests/probe_posix_elf_audit_test.py'):
        subprocess.run([sys.executable,path],cwd=ROOT,check=True)
    print('PASS T27 policy rejection checks='+str(rejected)+' plus unchanged stack/POSIX negative suites')
    return 0

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=('posix','stack','stream-posix','stream-stack','selftest'))
    parser.add_argument('--elf',type=Path);parser.add_argument('--output',type=Path)
    parser.add_argument('--source-root',type=Path,default=ROOT);parser.add_argument('--su-root',type=Path)
    parser.add_argument('--objdump');parser.add_argument('--disassembly',type=Path)
    args=parser.parse_args(argv)
    if args.mode=='selftest':return negative_tests()
    if not args.elf or not args.output:parser.error('--elf and --output required')
    try:
        elf=args.elf;output=args.output;output.parent.mkdir(parents=True,exist_ok=True)
        su_root=args.su_root or elf.parent;source_root=args.source_root
        dump=t26.legacy.read_linked_disassembly(elf,args.disassembly,args.objdump)
        probe=not args.mode.startswith('stream-')
        if args.mode.endswith('posix'):result=posix_audit(elf,output,source_root,dump,probe)
        else:
            policy_path=output.with_name(output.stem.replace('-audit','')+'-policy.json')
            if probe:policy=t26.prepare(elf,su_root,policy_path,source_root,args.disassembly,args.objdump)
            else:policy=stream.prepare(elf,su_root,policy_path,source_root)
            symbols=stack.elf_symbols(elf.read_bytes());functions=stack.parse_disassembly(dump)
            policy=extend_policy(policy,elf,su_root,policy_path,source_root,symbols,functions,probe)
            policy_path.write_text(json.dumps(policy,indent=2,ensure_ascii=False)+'\n',encoding='utf8')
            linked=output.with_name(output.stem+'-disassembly.txt');linked.write_text(dump,encoding='utf8')
            rc=stack.main(['--elf',str(elf),'--policy',str(policy_path),'--disassembly',str(linked),'--output',str(output)])
            if rc:return rc
            result=json.loads(output.read_text(encoding='utf8'))
        output.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf8')
        print('T27 '+args.mode+': '+('PASS' if result['passed'] else 'FAIL'))
        return 0 if result['passed'] else 1
    except (OSError,ValueError,KeyError,UnicodeError,subprocess.CalledProcessError) as error:
        result={'passed':False,'mode':args.mode,'issues':[{'kind':'invalid_or_incomplete_evidence','reason':str(error)}]}
        args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
        print('T27 native gate failed: '+str(error),file=sys.stderr);return 2

if __name__=='__main__':raise SystemExit(main())
