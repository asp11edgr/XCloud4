#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline non-executable ELF fixtures for the isolated T26 build gates."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_probe_t26_stack_policy as policy
import audit_probe_t26_posix_elf as posix

def elf_fixture(names, imports=()):
    strings, table = b"\0", b"\0" * 24
    for index, name in enumerate(names):
        at = len(strings);strings += name.encode() + b"\0"
        table += struct.pack("<IBBHQQ", at, 0x12, 0, 1, 0x100+index*0x100, 128)
    for name in imports:
        at = len(strings);strings += name.encode() + b"\0"
        table += struct.pack("<IBBHQQ", at, 0x12, 0, 0, 0, 0)
    # The resolver name is ordinary readonly data, not an imported SDK call.
    strings += b"getrusage\0"
    section_at = 64+len(strings)+len(table)
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01"+b"\0"*10,
        3, 62, 1, 0x100, 0, section_at, 0, 64, 0, 0, 64, 3, 0)
    sections = b"\0"*64
    sections += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, 64, len(strings), 0, 0, 1, 0)
    sections += struct.pack("<IIQQQQIIQQ", 0, 2, 0, 0, 64+len(strings), len(table), 1, 1, 8, 24)
    return header+strings+table+sections

def fixtures():
    names = sorted({entry for _,entry in policy.ENTRIES} | set(policy.REQUIRED_ROOTS) | set(policy.FILE_ROOTS) |
        {target for _,_,_,target in policy.BINDINGS} | {owner for owner,_,_,_ in policy.BINDINGS} |
        {"ascending", "x4_t26_samples_stats", "x4_t26_clip_load", "source_open", "x4_probe_source_open_fully_verified", "module_v1"})
    imports = tuple(policy.legacy.NATIVE_IMPORTS) + ("fstat", "sceKernelFsync")
    body = elf_fixture(names, imports);symbols = policy.gate.elf_symbols(body);lines=[]
    def target(name):return f'{symbols[name]["address"]:x} <{name}>'
    instructions={name:["ret"] for name in names}
    for owner,_,member,name in policy.BINDINGS:
        if instructions[owner]==["ret"]:instructions[owner]=[]
        instructions[owner].append("lea 0(%rip),%rax # "+target(name))
    for owner,_,_,_ in policy.BINDINGS:
        if instructions[owner][-1]!="ret":instructions[owner].append("ret")
    instructions["bank_worker"].insert(-1,"call "+target("x4_t26_clip_load"))
    instructions["x4_t26_samples_stats"]=["lea 0(%rip),%rax # "+target("ascending"),"ret"]
    instructions["x4_t26_clip_load"]=["lea 0(%rip),%r9 # "+target("x4_probe_hash_file"),
        "call "+target("x4_probe_source_open_fully_verified"),"ret"]
    instructions["x4_probe_source_open_fully_verified"]=["jmp "+target("source_open")]
    instructions["source_open"]=["mov %r9,%r15","call *%r15","call *%r15","ret"]
    instructions["x4_t26_cpu_snapshot_with"]=["call *%rax","ret"]
    instructions["module_v1"]=["call *%rbx","ret"]
    instructions["x4_probe_thread_trampoline"]=["call *%rax","ret"]
    for name in names:
        at=symbols[name]["address"];lines.append(f"{at:x} <{name}>:")
        for index,ins in enumerate(instructions[name]):lines.append(f" {at+index*8:x}: {ins}")
    for index,name in enumerate(imports):
        at=0x100000+index*0x100;lines.extend([f"{at:x} <{name}@plt>:",f" {at:x}: jmp *0x0(%rip)"])
    return body,"\n".join(lines)+"\n",symbols

class Tests(unittest.TestCase):
    def make_fixture(self, directory):
        root=Path(directory);build=root/"build";build.mkdir();body,dump,symbols=fixtures()
        (build/"bank.elf").write_bytes(body);(build/"bank.dump").write_text(dump,encoding="utf8")
        units={}
        for owner,unit,_,target in policy.BINDINGS:units[owner]=units[target]=unit
        units.update(x4_t26_samples_stats="probe_t26_metrics.c",ascending="probe_t26_metrics.c",
            x4_t26_clip_load="probe_t26_cache.c",x4_t26_cpu_snapshot_with="probe_t26_platform.c")
        mapping=[]
        for name,row in symbols.items():
            if row['undefined']:continue
            unit=units.get(name,'other.c');at=row['address'];size=row['size']
            mapping.extend([f'{at:x} {at:x} {size:x} 16 build/video/{unit[:-2]}.o:(.text.{name})',
                f'{at:x} {at:x} {size:x} 1 {name}'])
        (build/'bank.map').write_text('\n'.join(mapping)+'\n',encoding='utf8')
        rows=[f'{ROOT.as_posix()}/src/video/probe_t26_controller.c:1:{name}\t32\tstatic' for name,row in symbols.items() if not row["undefined"]]
        (build/"bank.su").write_text("\n".join(rows)+"\n",encoding="utf8")
        return root,build,body,dump,symbols
    def test_source_parser_ignores_strings_comments_and_nested_if(self):
        text='static int f(void) { if (1) { const char *x="}"; /* { */ return call(0); } }'
        table=policy.source_functions(text)
        self.assertEqual(set(table),{"f"});self.assertIn("call(0)",table["f"]["body"])
    def test_policy_binds_same_elf_su_source_callbacks_and_hash_sites(self):
        with tempfile.TemporaryDirectory() as directory:
            root,build,body,dump,symbols=self.make_fixture(directory)
            result=policy.prepare(build/"bank.elf",build,build/"policy.json",disassembly=build/"bank.dump")
            policy.gate.validate_policy(result,hashlib.sha256(body).hexdigest(),build)
            self.assertEqual(result["max_fraction"],.5);self.assertEqual(result["individual_frame_limit_bytes"],8192)
            self.assertEqual(result["opaque_allowance_bytes"],16384);self.assertEqual(len(result["sites"]),2)
            self.assertEqual(result["hash_callback_disassembly_sha256"],hashlib.sha256(dump.encode()).hexdigest())
            self.assertEqual(len(result["t26_callback_bindings"]),17)
            self.assertIsNone(result["threads"][0]["stack_bytes"])
            self.assertTrue(all(t["stack_bytes"]==262144 for t in result["threads"][1:]))
            self.assertTrue(all(row["allowance_bytes"]==16384 and not row["proven_runtime_bound"] for row in result["t26_indirect_function_reviews"]))
            usage=policy.gate.parse_stack_usage((build/"bank.su").read_text(),str(build/"bank.su"))
            audited=policy.gate.audit(symbols,policy.gate.parse_disassembly(dump),usage,result)
            self.assertTrue(audited["passed"],audited["issues"])
    def test_missing_callback_address_and_hash_register_clobber_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            _,build,body,dump,symbols=self.make_fixture(directory)
            altered=dump.replace("lea 0(%rip),%rax # "+f'{symbols["observe_decode"]["address"]:x} <observe_decode>',"nop")
            (build/"bank.dump").write_text(altered)
            with self.assertRaisesRegex(ValueError,"callback address proof"):
                policy.prepare(build/"bank.elf",build,build/"policy.json",disassembly=build/"bank.dump")
            altered=dump.replace("mov %r9,%r15","mov %r8,%r15")
            (build/"bank.dump").write_text(altered)
            with self.assertRaisesRegex(ValueError,"Unproved hash callback"):
                policy.prepare(build/"bank.elf",build,build/"policy.json",disassembly=build/"bank.dump")
    def test_link_map_name_size_and_source_ownership_are_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            _,build,body,dump,symbols=self.make_fixture(directory)
            text=(build/'bank.map').read_text()
            changed=text.replace('probe_t26_realistic.o:(.text.observe_decode)','other.o:(.text.observe_decode)')
            (build/'bank.map').write_text(changed)
            with self.assertRaisesRegex(ValueError,'Callback target link-map/source mismatch'):
                policy.prepare(build/'bank.elf',build,build/'policy.json',disassembly=build/'bank.dump')
    def test_unreviewed_indirect_owner_fails_without_global_permission(self):
        body,dump,symbols=fixtures();functions=policy.gate.parse_disassembly(dump)
        fn=functions["x4_t26_samples_stats"];fn["sites"].append({"address":fn["address"]+16,"kind":"call","indirect":True,"instruction":"call *%rax"})
        with self.assertRaisesRegex(ValueError,"Unreviewed T26 indirect owner"):
            policy.indirect_reviews(symbols,functions,ROOT)
    def test_readonly_bounded_local_jump_table_proof(self):
        data=struct.pack("<iii",0x1010-0x4000,0x1020-0x4000,0x1010-0x4000)
        section_at=64+len(data)
        header=struct.pack("<16sHHIQQQIHHHHHH",b"\x7fELF\x02\x01"+b"\0"*10,3,62,1,0x1000,0,section_at,0,64,0,0,64,2,0)
        body=header+data+b"\0"*64+struct.pack("<IIQQQQIIQQ",0,1,2,0x4000,64,len(data),0,0,4,0)
        dump="""1000 <local>:
1000: cmp $0x2,%eax
1001: ja 1020 <local+0x20>
1002: lea 0(%rip),%rcx # 4000 <table>
1003: movslq (%rcx,%rax,4),%rax
1004: add %rcx,%rax
1005: jmp *%rax
1010: ret
1020: ret
"""
        fn=policy.gate.parse_disassembly(dump)["local"];site=fn["sites"][0]
        proof=policy.local_jump_table(fn,site,body)
        self.assertEqual(proof["entries"],3);self.assertEqual(proof["target_addresses"],[0x1010,0x1020,0x1010])
        bad=bytearray(body);struct.pack_into("<i",bad,64,0x2000-0x4000)
        with self.assertRaisesRegex(ValueError,"leaves its linked function"):policy.local_jump_table(fn,site,bytes(bad))
        bad=bytearray(body);struct.pack_into("<Q",bad,section_at+64+8,3)
        with self.assertRaisesRegex(ValueError,"readonly"):policy.local_jump_table(fn,site,bytes(bad))
        fn=policy.gate.parse_disassembly(dump.replace("cmp $0x2,%eax","cmp $0x80,%eax"))["local"]
        self.assertIsNone(policy.local_jump_table(fn,fn["sites"][0],body))
    def test_individual_and_dynamic_frames_still_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            _,build,body,dump,symbols=self.make_fixture(directory)
            result=policy.prepare(build/"bank.elf",build,build/"policy.json",disassembly=build/"bank.dump")
            usage=policy.gate.parse_stack_usage((build/"bank.su").read_text())
            usage["bank_worker"][0]["bytes"]=8193
            audited=policy.gate.audit(symbols,policy.gate.parse_disassembly(dump),usage,result)
            self.assertFalse(audited["passed"]);self.assertTrue(any(x["kind"]=="individual_frame_limit_exceeded" for x in audited["issues"]))
            usage["bank_worker"][0]["bytes"]=32;usage["bank_worker"][0]["qualifier"]="dynamic"
            audited=policy.gate.audit(symbols,policy.gate.parse_disassembly(dump),usage,result)
            self.assertFalse(audited["passed"]);self.assertTrue(any(x["kind"]=="dynamic_stack_forbidden" for x in audited["issues"]))
    def test_posix_reuses_enosys_gate_and_cpu_pfn_proof(self):
        body,dump,symbols=fixtures();functions=policy.gate.parse_disassembly(dump)
        result=posix.audit(symbols,functions,ROOT,body)
        self.assertTrue(result["passed"],result);self.assertEqual(result["required_native_imports"],["fstat","sceKernelFsync"])
        self.assertEqual(result["t26_dynamic_cpu_proof"]["who"],[0,1]);self.assertEqual(result["t26_dynamic_cpu_proof"]["rusage_bytes"],144)
        symbols["mkdirat"]={"name":"mkdirat","address":0,"size":0,"undefined":True,"symbol_kind":2,"symbol_binding":1}
        result=posix.audit(symbols,functions,ROOT,body)
        self.assertFalse(result["passed"]);self.assertIn("mkdirat",result["forbidden_imports"])
    def test_posix_callback_route_to_stub_and_missing_fsync_fail(self):
        body,dump,symbols=fixtures();functions=policy.gate.parse_disassembly(dump)
        symbols["fstatat"]={"name":"fstatat","address":0x200000,"size":8,"undefined":False,"symbol_kind":2,"symbol_binding":1}
        functions["fstatat"]={"name":"fstatat","address":0x200000,"instructions":[],"sites":[]}
        functions["observe_flip"]["instructions"]=[{"address":symbols["observe_flip"]["address"],"mnemonic":"call","operands":"200000 <fstatat>","text":"call 200000 <fstatat>"}]
        result=posix.audit(symbols,functions,ROOT,body)
        self.assertFalse(result["passed"]);self.assertIn("fstatat",{x["name"] for x in result["reachable_linked_stubs"]})
        body,dump,symbols=fixtures();del symbols["sceKernelFsync"]
        result=posix.audit(symbols,policy.gate.parse_disassembly(dump),ROOT,body)
        self.assertFalse(result["passed"]);self.assertIn("sceKernelFsync",result["missing_required_native_imports"])
    def test_posix_homonyms_remain_separate_by_address(self):
        dump="100 <pace>:\n100: call 300 <fstatat>\n108: ret\n200 <pace>:\n200: ret\n300 <fstatat>:\n300: ret\n"
        functions=policy.gate.parse_disassembly(dump);blocks=posix.posix_blocks({},functions)
        self.assertEqual(blocks["pace@0x100"]["references"],{"fstatat"})
        self.assertEqual(blocks["pace@0x200"]["references"],set())
    def test_static_getrusage_route_is_not_a_cpu_measurement(self):
        body,dump,symbols=fixtures();functions=policy.gate.parse_disassembly(dump)
        symbols["getrusage"]={"name":"getrusage","address":0x200000,"size":8,"undefined":False,"symbol_kind":2,"symbol_binding":1}
        functions["getrusage"]={"name":"getrusage","address":0x200000,"instructions":[],"sites":[]}
        functions["x4_t26_cpu_snapshot_with"]["instructions"]=[{"address":symbols["x4_t26_cpu_snapshot_with"]["address"],"mnemonic":"call","operands":"200000 <getrusage>","text":"call 200000 <getrusage>"}]
        result=posix.audit(symbols,functions,ROOT,body)
        self.assertFalse(result["passed"]);self.assertEqual(result["forbidden_static_cpu_routes"],["getrusage"])

if __name__=="__main__":unittest.main()
