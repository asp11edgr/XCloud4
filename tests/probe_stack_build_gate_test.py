#!/usr/bin/env python3
"""Offline policy/packaging integration fixtures; native ELF is not executed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_probe_stack", ROOT / "scripts/prepare_probe_stack_policy.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)
gate = prepare.gate


def elf_fixture(names=("_start", "main", "video_worker", "copy_worker", "probe_stack_test_entry", "x4_probe_thread_trampoline",
    "probe_default_stack_entry", "native_allocate", "native_map", "native_unmap", "native_release",
    "native_heap_allocate", "native_heap_free", "native_clock", "memory_progress", "x4_probe_hash_file",
    "source_open", "x4_probe_source_open_fully_verified", "x4_probe_reference_open_verified", "start_configured",
    "probe_pipeline_decode_worker", "probe_pipeline_convert_worker")):
    """Minimal ELF64 symbol fixture; not a runnable PS4 binary."""
    strings = b"\0"
    symbols = b"\0" * 24
    for i, name in enumerate(names):
        offset = len(strings)
        strings += name.encode() + b"\0"
        symbols += struct.pack("<IBBHQQ", offset, 0x12, 0, 1, 0x100 + 0x100 * i, 16)
    section_at = 64 + len(strings) + len(symbols)
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01" + b"\0" * 10,
        3, 62, 1, 0x100, 0, section_at, 0, 64, 0, 0, 64, 3, 0)
    sections = b"\0" * 64
    sections += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, 64, len(strings), 0, 0, 1, 0)
    sections += struct.pack("<IIQQQQIIQQ", 0, 2, 0, 0, 64 + len(strings), len(symbols), 1, 1, 8, 24)
    return header + strings + symbols + sections

def hash_dump(symbols):
    """Small SysV argument/CFG fixture; deliberately not a native executable."""
    out = []
    def emit(name, instructions):
        at = symbols[name]["address"]
        out.append(f'{at:x} <{name}>:')
        for offset, text in instructions:
            out.append(f' {at + offset:x}: {text}')
    def target(name): return f'{symbols[name]["address"]:x} <{name}>'
    emit('video_worker', [(0,'lea 0(%rip),%r9 # '+target('x4_probe_hash_file')),
        (7,'call '+target('x4_probe_source_open_fully_verified')), (12,'ret')])
    emit('probe_pipeline_decode_worker', [(0,'lea 0(%rip),%r9 # '+target('x4_probe_hash_file')),
        (7,'call '+target('x4_probe_source_open_fully_verified')), (12,'ret')])
    emit('start_configured', [(0,'lea 0(%rip),%rax # '+target('x4_probe_hash_file')),
        (7,'push %rax'), (8,'call '+target('x4_probe_reference_open_verified')), (13,'ret')])
    emit('x4_probe_source_open_fully_verified', [(0,'jmp '+target('source_open'))])
    emit('source_open', [(0,'mov %r9,%r15'), (3,'call *%r15'), (6,'call *%r15'), (9,'ret')])
    emit('x4_probe_reference_open_verified', [(0,'push %rbp'), (1,'mov %rsp,%rbp'),
        (4,'mov 0x10(%rbp),%r12'), (8,'call *%r12'), (11,'ret')])
    emit('x4_probe_hash_file', [(0,'ret')])
    return '\n'.join(out)+'\n'


def find_bash():
    tool = shutil.which("bash")
    if tool:
        return tool
    windows_git = Path("C:/Program Files/Git/bin/bash.exe")
    return str(windows_git) if windows_git.is_file() else None


def probe_audit_branches(make):
    """Validate each selected recipe within the actual nested probe guard.

    Searching the first audit occurrence in the whole Makefile confuses the
    nested T27/T26/legacy branches. Preserve every historical recipe and check
    the T27 frontend exactly; it prepares and audits the same ELF internally.
    """
    lines = make.splitlines(keepends=True)
    prefix = ["ifeq ($(X4_VIDEO_PROBE),1)\n",
              ".PHONY: probe-posix-audit probe-stack-audit\n"]
    starts = [i for i in range(len(lines)-1) if lines[i:i+2] == prefix]
    if len(starts) != 1:
        raise ValueError("Missing unique guarded probe audit block")

    def conditional(block, at, selector):
        if at >= len(block) or block[at].rstrip('\n') != selector:
            raise ValueError("Missing exact audit selector: "+selector)
        depth = 1; after_else = False; parts = [[], []]
        for i in range(at+1, len(block)):
            directive = block[i].rstrip('\n')
            if re.match(r"^(?:ifeq|ifneq|ifdef|ifndef)\b", directive):
                depth += 1
            elif directive == "endif":
                depth -= 1
                if depth == 0:
                    if not after_else:
                        raise ValueError("Missing audit alternative: "+selector)
                    return parts[0], parts[1], i+1
            elif depth == 1 and directive.startswith("else"):
                if directive != "else" or after_else:
                    raise ValueError("Unexpected audit alternative: "+selector)
                after_else = True; continue
            parts[int(after_else)].append(block[i])
        raise ValueError("Unclosed audit selector: "+selector)

    t26_block, legacy, following = conditional(lines, starts[0]+2, "ifeq ($(X4_PROBE_T26),1)")
    if following >= len(lines) or lines[following] != "build/eboot.bin: probe-posix-audit probe-stack-audit\n":
        raise ValueError("Missing common current SELF dependencies")
    meaningful = [i for i, line in enumerate(t26_block) if line.strip() and not line.startswith('#')]
    if not meaningful:
        raise ValueError("Missing nested T27 selector")
    t27, t26, end = conditional(t26_block, meaningful[0], "ifeq ($(X4_PROBE_T27),1)")
    if any(line.strip() and not line.startswith('#') for line in t26_block[end:]):
        raise ValueError("Unexpected recipe outside the T27/T26 branches")
    branches = {"t27": ''.join(t27), "t26": ''.join(t26), "legacy": ''.join(legacy)}

    def exact_recipe(branch, target, dependencies, commands, name):
        pattern = r"(?m)^"+re.escape(target)+r": build/xcloud4\.elf (?P<deps>[^\n]+)\n(?P<commands>(?:\t[^\n]*\n)+)"
        recipes = list(re.finditer(pattern, branch))
        headers = re.findall(r"(?m)^"+re.escape(target)+r":", branch)
        if len(headers) != 1 or len(recipes) != 1 or recipes[0]["deps"].split() != dependencies:
            raise ValueError("Audit must depend on its actual preparer and auditor: "+name+":"+target)
        actual = [line.strip() for line in recipes[0]["commands"].splitlines()]
        if actual != commands:
            raise ValueError("Branch must audit the same current ELF/policy: "+name+":"+target)

    for name, branch in branches.items():
        if name == "t27":
            for mode in ("posix", "stack"):
                exact_recipe(branch, "probe-"+mode+"-audit", ["scripts/test_t27_native_gates.py"],
                    ["python3 scripts/test_t27_native_gates.py "+mode+
                     " --elf build/xcloud4.elf --output build/probe-"+mode+"-audit.json"], name)
            continue
        prepare_name = "prepare_probe_t26_stack_policy.py" if name == "t26" else "prepare_probe_stack_policy.py"
        expected = [
            "python3 scripts/"+prepare_name+" --elf build/xcloud4.elf --su-root build --output build/probe-stack-policy.json",
            "python3 scripts/audit_probe_stack.py --elf build/xcloud4.elf --policy build/probe-stack-policy.json --objdump objdump --output build/probe-stack-audit.json",
        ]
        exact_recipe(branch, "probe-stack-audit", ["scripts/audit_probe_stack.py", "scripts/"+prepare_name], expected, name)
        audit_name = "audit_probe_t26_posix_elf.py" if name == "t26" else "audit_probe_posix_elf.py"
        deps = ["scripts/"+audit_name]
        if name == "t26": deps += ["scripts/prepare_probe_t26_stack_policy.py", "scripts/audit_probe_posix_elf.py"]
        exact_recipe(branch, "probe-posix-audit", deps,
            ["python3 scripts/"+audit_name+" build/xcloud4.elf --objdump objdump --output build/probe-posix-audit.json"], name)
    return branches


class Tests(unittest.TestCase):
    def make_fixture(self, directory):
        root = Path(directory)
        build = root / "build"
        build.mkdir()
        (build / "xcloud4.elf").write_bytes(elf_fixture())
        (build / "probe.su").write_text("probe.c:1:_start\t32\tstatic\nprobe.c:2:video_worker\t32\tstatic\nprobe.c:3:copy_worker\t32\tstatic\n", encoding="utf-8")
        patch = mock.patch.object(prepare, "read_linked_disassembly", side_effect=lambda elf, *_: hash_dump(gate.elf_symbols(Path(elf).read_bytes())))
        patch.start(); self.addCleanup(patch.stop)
        return root, build

    def test_policy_binds_elf_and_relative_su_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy = prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")
            self.assertEqual(policy["elf_sha256"], hashlib.sha256((build / "xcloud4.elf").read_bytes()).hexdigest())
            self.assertEqual(policy["stack_usage_files"][0]["path"], "probe.su")
            self.assertEqual(policy["stack_usage_files"][0]["sha256"], hashlib.sha256((build / "probe.su").read_bytes()).hexdigest())

    def test_policy_has_explicit_workers_and_unknown_main_and_default_info(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy = prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")
            self.assertEqual([t["name"] for t in policy["threads"]],
                ["main", "x4-video", "x4-copy0", "x4-copy1", "x4-copy2", "x4-stack-check", "x4-stack-default",
                 "x4-pipeline-decode", "x4-pipeline-convert0", "x4-pipeline-convert1", "x4-pipeline-convert2"])
            self.assertIsNone(policy["threads"][0]["stack_bytes"])
            self.assertTrue(all(t["stack_bytes"] == 262144 for t in policy["threads"][1:6]))
            self.assertTrue(all(t["wrapper_entry"] == "x4_probe_thread_trampoline" for t in policy["threads"][1:6]))
            self.assertTrue(all(t["stack_bytes"] == 262144 for t in policy["threads"][7:]))
            self.assertTrue(all(t["wrapper_entry"] == "x4_probe_thread_trampoline" for t in policy["threads"][7:]))
            info = policy["threads"][6]
            self.assertEqual(info["entry"], "probe_default_stack_entry")
            self.assertIsNone(info["stack_bytes"])
            self.assertIsNone(info["wrapper_entry"])
            self.assertTrue(info["informational_only"])
            self.assertEqual(info["evidence"]["kind"], "informational_null_attributes")
            gate.validate_policy(policy, policy["elf_sha256"], build)
            self.assertEqual(policy["opaque_allowance_bytes"], 16384)
            self.assertEqual(policy["individual_frame_limit_bytes"], 8192)
            self.assertEqual(policy["shared_recursion"][0]["max_visits"], 2)
            self.assertIn("scePthreadAttrSetstacksize", policy["required_native_imports"])
            self.assertEqual(len(policy["sites"]), 3)
            self.assertEqual(policy["additional_roots"], ["native_allocate", "native_map", "native_unmap", "native_release",
                "native_heap_allocate", "native_heap_free", "native_clock", "memory_progress", "x4_probe_hash_file"])

    def test_hash_root_binds_callers_and_body_with_three_validated_sites(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy = prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")
            self.assertIn("x4_probe_hash_file", policy["additional_roots"])
            evidence = {Path(item["artifact"]).name: item for item in policy["source_evidence"]}
            for relative in ["src/media/live_media.c", "src/video/live_h264.c", "src/video/probe_hash.c", "src/video/probe_pipeline.c"]:
                path = ROOT / relative
                self.assertEqual(evidence[path.name]["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(len(policy["sites"]), 3)
            self.assertTrue(all(site['targets']==['x4_probe_hash_file'] for site in policy['sites'].values()))
            self.assertTrue(any("Exactly three production hash dispatches are resolved" in line for line in policy["limitations"]))

    def test_missing_or_undefined_hash_callback_body_fails(self):
        for undefined in [False, True]:
            with self.subTest(undefined=undefined), tempfile.TemporaryDirectory() as directory:
                root, build = self.make_fixture(directory)
                symbols = gate.elf_symbols((build / "xcloud4.elf").read_bytes())
                if undefined:
                    symbols["x4_probe_hash_file"]["undefined"] = True
                else:
                    symbols.pop("x4_probe_hash_file")
                with mock.patch.object(gate, "elf_symbols", return_value=symbols):
                    with self.assertRaisesRegex(ValueError, "x4_probe_hash_file"):
                        prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_changed_hash_callback_caller_fails(self):
        for relative in ["src/media/live_media.c", "src/video/live_h264.c", "src/video/probe_pipeline.c"]:
            with self.subTest(caller=relative), tempfile.TemporaryDirectory() as directory:
                root, build = self.make_fixture(directory)
                original_read = Path.read_text
                def changed_read(path, *args, **kwargs):
                    value = original_read(path, *args, **kwargs)
                    return value.replace("x4_probe_hash_file", "different_callback") if path == ROOT / relative else value
                with mock.patch.object(Path, "read_text", changed_read):
                    with self.assertRaisesRegex(ValueError, "Reviewed hash callback is missing"):
                        prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_hash_linked_proof_resolves_only_three_calls(self):
        symbols = gate.elf_symbols(elf_fixture())
        sites = prepare.hash_callback_sites(symbols, gate.parse_disassembly(hash_dump(symbols)))
        self.assertEqual([name for name, _ in sites], ['source_open', 'source_open', 'x4_probe_reference_open_verified'])

    def test_hash_linked_proof_rejects_wrong_address_arguments_and_clobbers(self):
        symbols = gate.elf_symbols(elf_fixture())
        dump = hash_dump(symbols)
        for before, after in [
            ('lea 0(%rip),%r9', 'lea 0(%rip),%r8'),
            ('mov %r9,%r15', 'mov %r8,%r15'),
            ('mov 0x10(%rbp),%r12', 'mov 0x18(%rbp),%r12'),
            ('push %rax', 'push %rbx'),
            ('call *%r15', 'call *%r14'),
            (f'# {symbols["x4_probe_hash_file"]["address"]:x} <x4_probe_hash_file>', '# dead <x4_probe_hash_file>')]:
            with self.subTest(change=after), self.assertRaises(ValueError):
                prepare.hash_callback_sites(symbols, gate.parse_disassembly(dump.replace(before, after)))

    def test_hash_callee_cfg_distinguishes_error_return_from_callback_path(self):
        symbols = gate.elf_symbols(elf_fixture()); functions = gate.parse_disassembly(hash_dump(symbols))
        base = symbols['source_open']['address']
        dump = f'''{base:x} <source_open>:
 {base:x}: mov %r9,%r15
 {base+3:x}: test %eax,%eax
 {base+5:x}: je {base+16:x} <source_open+0x10>
 {base+7:x}: call *%r15
 {base+10:x}: call *%r15
 {base+13:x}: ret
 {base+16:x}: mov $1,%r15b
 {base+20:x}: ret
'''
        functions['source_open'] = gate.parse_disassembly(dump)['source_open']
        self.assertEqual(len(prepare.hash_callback_sites(symbols, functions)), 3)
        # The same low-byte overwrite must fail if that branch rejoins a call.
        functions['source_open'] = gate.parse_disassembly(dump.replace(f'{base+20:x}: ret',
            f'{base+20:x}: jmp {base+7:x} <source_open+0x7>'))['source_open']
        with self.assertRaisesRegex(ValueError, 'Unproved hash callback register'):
            prepare.hash_callback_sites(symbols, functions)

    def test_hash_wrapper_clobber_and_extra_caller_fail(self):
        symbols = gate.elf_symbols(elf_fixture()); dump = hash_dump(symbols)
        wrapper = symbols['x4_probe_source_open_fully_verified']['address']
        changed = dump.replace(f'{wrapper:x}: jmp', f'{wrapper:x}: xor %r9d,%r9d\n {wrapper+3:x}: jmp')
        with self.assertRaisesRegex(ValueError, 'wrapper'):
            prepare.hash_callback_sites(symbols, gate.parse_disassembly(changed))
        extra = f'ffff0 <unexpected>:\n ffff0: call {symbols["x4_probe_reference_open_verified"]["address"]:x} <x4_probe_reference_open_verified>\n ffff5: ret\n'
        with self.assertRaisesRegex(ValueError, 'caller'):
            prepare.hash_callback_sites(symbols, gate.parse_disassembly(dump+extra))

    def test_hash_proof_disassembly_change_blocks_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy = prepare.prepare(build / 'xcloud4.elf', build, build / 'policy.json')
            (build / 'policy.json').write_text(json.dumps(policy), encoding='utf-8')
            (build / 'dis.txt').write_text(hash_dump(gate.elf_symbols((build / 'xcloud4.elf').read_bytes()))+'\n', encoding='utf-8')
            self.assertEqual(gate.main(['--elf',str(build/'xcloud4.elf'),'--policy',str(build/'policy.json'),
                '--disassembly',str(build/'dis.txt'),'--output',str(build/'gate.json')]),1)
            result=json.loads((build/'gate.json').read_text())
            self.assertIn('different linked disassembly',result['issues'][0]['reason'])

    def test_missing_null_attribute_info_entry_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            names = [name for name in gate.elf_symbols((build / "xcloud4.elf").read_bytes()) if name != "probe_default_stack_entry"]
            (build / "xcloud4.elf").write_bytes(elf_fixture(names))
            with self.assertRaisesRegex(ValueError, "probe_default_stack_entry"):
                prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_missing_pipeline_thread_entry_fails(self):
        for entry in ("probe_pipeline_decode_worker", "probe_pipeline_convert_worker"):
            with self.subTest(entry=entry), tempfile.TemporaryDirectory() as directory:
                root, build = self.make_fixture(directory)
                names = [name for name in gate.elf_symbols((build / "xcloud4.elf").read_bytes()) if name != entry]
                (build / "xcloud4.elf").write_bytes(elf_fixture(names))
                with self.assertRaisesRegex(ValueError, entry):
                    prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_missing_indirect_memory_callback_body_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            names = [name for name in gate.elf_symbols((build / "xcloud4.elf").read_bytes()) if name != "native_map"]
            (build / "xcloud4.elf").write_bytes(elf_fixture(names))
            with self.assertRaisesRegex(ValueError, "native_map"):
                prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_missing_crt_entry_cannot_use_main_as_substitute(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            (build / "xcloud4.elf").write_bytes(elf_fixture(("main", "video_worker", "copy_worker")))
            with self.assertRaisesRegex(ValueError, "_start"):
                prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_real_elf_parser_preserves_duplicate_local_names(self):
        symbols = gate.elf_symbols(elf_fixture(("_start", "stage", "stage")))
        self.assertNotIn("stage", symbols)
        self.assertEqual(symbols["stage@0x200"]["name"], "stage")
        self.assertEqual(symbols["stage@0x300"]["address"], 0x300)

    def test_missing_copy_entry_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            (build / "xcloud4.elf").write_bytes(elf_fixture(("_start", "video_worker")))
            with self.assertRaisesRegex(ValueError, "copy_worker"):
                prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_missing_su_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            (build / "probe.su").unlink()
            with self.assertRaisesRegex(ValueError, "No same-build"):
                prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")

    def test_policy_cli_real_fixture_and_gate_fail_no_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy_path = build / "policy.json"
            self.assertEqual(prepare.main(["--elf", str(build / "xcloud4.elf"), "--su-root", str(build), "--output", str(policy_path)]), 0)
            (build / "elf-disassembly.txt").write_text(hash_dump(gate.elf_symbols((build / "xcloud4.elf").read_bytes())), encoding="utf-8")
            self.assertEqual(gate.main(["--elf", str(build / "xcloud4.elf"), "--policy", str(policy_path),
                "--disassembly", str(build / "elf-disassembly.txt"), "--output", str(build / "gate.json")]), 1)
            result = json.loads((build / "gate.json").read_text())
            self.assertFalse(result["passed"])
            self.assertEqual(len(result["threads"]), 11)
            self.assertTrue(any(i["kind"] == "expected_native_import_not_verified" for i in result["issues"]))

    def test_stale_su_hash_blocks_replay_of_prior_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            policy = prepare.prepare(build / "xcloud4.elf", build, build / "policy.json")
            (build / "policy.json").write_text(json.dumps(policy), encoding="utf-8")
            (build / "probe.su").write_text("probe.c:1:_start\t65536\tstatic\n", encoding="utf-8")
            (build / "dis.txt").write_text(hash_dump(gate.elf_symbols((build / "xcloud4.elf").read_bytes())), encoding="utf-8")
            self.assertEqual(gate.main(["--elf", str(build / "xcloud4.elf"), "--policy", str(build / "policy.json"),
                "--disassembly", str(build / "dis.txt"), "--output", str(build / "gate.json")]), 1)
            result = json.loads((build / "gate.json").read_text())
            self.assertIn("stale/mismatched .su", result["issues"][0]["reason"])

    def test_probe_compiler_flag_precedes_cxx_snapshot(self):
        make = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("ifneq ($(filter 1,$(X4_VIDEO_PROBE))$(filter 1 2,$(X4_STREAM_PROFILE)),)\nCFLAGS += -fstack-usage -Werror=vla -Werror=alloca\nendif", make)
        self.assertLess(make.index("CFLAGS += -fstack-usage"), make.index("CXXFLAGS :="))

    def test_self_requires_both_current_phony_gates(self):
        make = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn(".PHONY: probe-posix-audit probe-stack-audit", make)
        self.assertIn("build/eboot.bin: probe-posix-audit probe-stack-audit", make)
        branches = probe_audit_branches(make)
        self.assertEqual(set(branches), {"t27", "t26", "legacy"})
        for mode in ("posix", "stack"):
            self.assertIn("python3 scripts/test_t27_native_gates.py "+mode+
                " --elf build/xcloud4.elf --output build/probe-"+mode+"-audit.json", branches["t27"])
        for name, prepare_name in (("t26", "prepare_probe_t26_stack_policy.py"),
                                   ("legacy", "prepare_probe_stack_policy.py")):
            with self.subTest(branch=name):
                branch = branches[name]
                self.assertIn("probe-stack-audit: build/xcloud4.elf", branch)
                self.assertLess(branch.index("scripts/"+prepare_name+" --elf"),
                                branch.index("scripts/audit_probe_stack.py --elf"))

    def test_each_audit_branch_rejects_reversed_or_wrong_preparer(self):
        make = (ROOT / "Makefile").read_text(encoding="utf-8")
        branches = probe_audit_branches(make)
        for name, branch in branches.items():
            if name == "t27":
                continue  # Its single frontend command is checked below.
            recipe = re.search(r"(?m)^probe-stack-audit:[^\n]+\n(?P<commands>(?:\t[^\n]*\n)+)", branch)
            commands = recipe["commands"].splitlines(keepends=True)
            self.assertEqual(len(commands), 2)
            changed = branch.replace(recipe["commands"], "".join(reversed(commands)), 1)
            with self.subTest(branch=name, mutation="audit_before_prepare"), self.assertRaises(ValueError):
                probe_audit_branches(make.replace(branch, changed, 1))
            own = "prepare_probe_t26_stack_policy.py" if name == "t26" else "prepare_probe_stack_policy.py"
            other = "prepare_probe_stack_policy.py" if name == "t26" else "prepare_probe_t26_stack_policy.py"
            changed = branch.replace(own, other)
            with self.subTest(branch=name, mutation="wrong_preparer"), self.assertRaises(ValueError):
                probe_audit_branches(make.replace(branch, changed, 1))

    def test_t27_frontend_wrong_elf_mode_output_or_dependency_fails(self):
        make = (ROOT / "Makefile").read_text(encoding="utf-8")
        branch = probe_audit_branches(make)["t27"]
        for before, after in (
            ("scripts/test_t27_native_gates.py stack --elf", "scripts/test_t27_native_gates.py posix --elf"),
            ("--elf build/xcloud4.elf", "--elf build/stale.elf"),
            ("--output build/probe-stack-audit.json", "--output build/stale-stack-audit.json"),
            ("probe-stack-audit: build/xcloud4.elf scripts/test_t27_native_gates.py",
             "probe-stack-audit: build/xcloud4.elf scripts/audit_probe_stack.py"),
            ("scripts/test_t27_native_gates.py", "scripts/prepare_probe_t26_stack_policy.py"),
        ):
            with self.subTest(mutation=after), self.assertRaises(ValueError):
                probe_audit_branches(make.replace(branch, branch.replace(before, after), 1))

    def test_audit_selector_and_shared_self_dependencies_cannot_be_removed(self):
        make = (ROOT / "Makefile").read_text(encoding="utf-8")
        for old, new in (
            ("ifeq ($(X4_PROBE_T26),1)\n# T26 has different", "ifneq ($(X4_PROBE_T26),0)\n# T26 has different"),
            ("ifeq ($(X4_PROBE_T27),1)\nprobe-posix-audit", "ifneq ($(X4_PROBE_T27),0)\nprobe-posix-audit"),
            ("build/eboot.bin: probe-posix-audit probe-stack-audit", "build/eboot.bin: probe-posix-audit"),
        ):
            with self.subTest(mutation=new), self.assertRaises(ValueError):
                probe_audit_branches(make.replace(old, new, 1))

    def test_direct_packaging_checks_gate_before_identity_or_staging(self):
        source = (ROOT / "scripts/empaquetar.sh").read_text(encoding="utf-8")
        self.assertLess(source.index("make build/eboot.bin X4_VIDEO_PROBE=1"), source.index("IDENTITY=$(python3"))
        self.assertLess(source.index("make build/eboot.bin X4_VIDEO_PROBE=1"), source.index('mkdir -p "$PACKAGE_ROOT'))
        self.assertIn('if [[ "$PACKAGE_MODE" == probe ]]; then', source)
        self.assertIn('X4_VIDEO_CAPACITY_1080="$PROBE_CAPACITY"', source)
        self.assertIn("set -Eeuo pipefail", source)

    def test_packaging_actual_shell_cannot_stage_old_self_after_gate_failure(self):
        bash = find_bash()
        if not bash:
            self.skipTest("bash is not installed; structural checks remain separate")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            shutil.copy2(ROOT / "scripts/empaquetar.sh", root / "scripts/empaquetar.sh")
            (root / "build").mkdir()
            (root / "build/eboot.bin").write_bytes(b"stale fixture SELF, never usable")
            tool = root / "tools"
            tool.mkdir()
            marker = root / "made-self.txt"
            (tool / "make").write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$*" > made-self.txt\nexit 41\n', encoding="utf-8")
            (tool / "make").chmod(0o755)
            env = dict(os.environ, OO_PS4_TOOLCHAIN=str(root / "sdk"), X4_PACKAGE_MODE="probe", X4_PROBE_VARIANT="off")
            env["PATH"] = str(tool) + os.pathsep + env.get("PATH", "")
            result = subprocess.run([bash, str(root / "scripts/empaquetar.sh")], cwd=root, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 41, msg=result.stderr)
            self.assertIn("X4_VIDEO_PROBE=1", marker.read_text())
            self.assertEqual((root / "build/eboot.bin").read_bytes(), b"stale fixture SELF, never usable")
            self.assertFalse((root / "build/package-probe-off").exists())
            self.assertFalse((root / "dist").exists())

    def test_make_actual_stale_self_and_earlier_pass_do_not_bypass(self):
        make = shutil.which("make") or shutil.which("gmake")
        if not make:
            self.skipTest("GNU make absent on this host; run fixture in compilation VM")
        with tempfile.TemporaryDirectory() as directory:
            root, build = self.make_fixture(directory)
            (root / "Makefile").write_text('''OO_PS4_TOOLCHAIN := fixture
.PHONY: all probe-stack-audit
all: build/eboot.bin
probe-stack-audit:
\t@echo failed-current-stack-gate > build/current-gate.txt
\t@exit 41
build/eboot.bin: build/xcloud4.elf probe-stack-audit
\t@echo UNSAFE-REPLACEMENT > $@
''', encoding="utf-8")
            (build / "eboot.bin").write_bytes(b"stale-self")
            (build / "probe-stack-audit.json").write_text('{"passed":true}', encoding="utf-8")
            # Identical prerequisite structure as the real Makefile. A phony
            # failure must prevent the target recipe despite both stale files.
            result = subprocess.run([make, "all"], cwd=root, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((build / "eboot.bin").read_bytes(), b"stale-self")
            self.assertTrue((build / "current-gate.txt").is_file())


if __name__ == "__main__":
    unittest.main()
