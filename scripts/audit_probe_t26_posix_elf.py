#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T26 roots and real callbacks over the unchanged strict POSIX/ENOSYS gate.

No target ELF is executed. Local homonyms are retained by linked address, so
the two pace/interrupted/before_close bodies cannot hide one another. Native
getrusage is a dynamically resolved PFN with144-byte LP64 storage; no static
SDK getrusage placeholder is accepted as functional CPU measurement.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import audit_probe_posix_elf as legacy
import audit_probe_stack as stack
import prepare_probe_t26_stack_policy as prepare

ROOT_NAMES = ("main", "_start", "x4_probe_controller_run", "x4_t26_controller_run",
    "bank_worker", "stream_pipeline_decode_worker", "stream_pipeline_convert_worker",
    "copy_worker", "x4_probe_thread_trampoline", "x4_probe_hash_file", "x4_t26_cpu_snapshot_with")
FILE_ROOTS = ("__stdio_read", "__stdio_write", "__stdio_seek", "__stdio_close")

def posix_blocks(symbols, functions):
    """Preserve exact linked target addresses while reusing legacy traversal."""
    by_address = {fn["address"]: name for name, fn in functions.items()}
    blocks = {}
    for name, fn in functions.items():
        refs, callsites, indirect = set(), [], 0
        for ins in fn["instructions"]:
            op, operands = ins["mnemonic"], ins["operands"]
            branch = op.startswith(("call", "jmp"))
            if branch and re.match(r'\*|(?:qword\s+ptr\s+)?\[', operands, re.I):indirect += 1
            for found in re.finditer(r'(?:0x)?([0-9a-fA-F]+)\s+<([^<>]+)>', operands):
                at, label = int(found[1], 16), found[2]
                base = stack.base_name(label)
                # Intra-function branch labels are not another function route.
                if base == fn["name"] and (at != fn["address"] or op.startswith("j")):continue
                if base.endswith("@plt"):base = base[:-4]
                target = by_address.get(at, base)
                if target.endswith("@plt"):target = target[:-4]
                if target != name:
                    refs.add(target)
                    if branch:callsites.append(target)
        blocks[name] = {"address": fn["address"], "references": refs,
            "indirect_instructions": indirect, "call_sites": callsites}
    return blocks

def cpu_proof(source_root, body, symbols):
    path = source_root / "src/video/probe_t26_platform.c"
    header = source_root / "src/video/probe_t26_platform.h"
    text, declarations = path.read_text(encoding="utf8"), header.read_text(encoding="utf8")
    required = ('x4_module_symbol(p->info.kernel_handle,"getrusage",&a)',
        'out->process_rc=call(0,&r)', 'out->thread_rc=call(1,&r)',
        'memset(&r,0xa5,sizeof(r))', 'out->process_available=out->process_rc==0',
        'out->thread_available=out->thread_rc==0')
    compact = re.sub(r'\s+', '', text)
    if any(re.sub(r'\s+', '', value) not in compact for value in required):
        raise ValueError("Typed dynamic getrusage/sentinel availability source changed")
    if not re.search(r'_Static_assert\s*\(sizeof\(X4T26Rusage\)\s*==\s*144', declarations):
        raise ValueError("getrusage LP64 size assertion missing")
    if b'getrusage\0' not in body:
        raise ValueError("Same ELF lacks the dynamic getrusage resolver string")
    return {"method": "getrusage PFN resolved from libkernel using x4_module_symbol",
        "who": [0, 1], "rusage_bytes": 144, "sentinel": "0xa5",
        "failure": "available=false; JSON null, never wall time or invented zero CPU",
        "static_sdk_symbol_present": "getrusage" in symbols,
        "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
        "interpretation": "ELF/source binding proves the typed resolver path, not firmware availability or a runtime CPU result."}

def audit(symbols, functions, source_root, body):
    callbacks = prepare.callback_bindings(symbols, functions, source_root)
    roots = tuple(prepare.linked_name(symbols, functions, name) for name in ROOT_NAMES)
    files = [prepare.linked_name(symbols, functions, name) for name in FILE_ROOTS]
    for optional in ("__stdout_write",):
        if prepare.symbols_named(symbols, optional):files.append(prepare.linked_name(symbols, functions, optional))
    indirect = tuple(sorted(set(files) | {row["target"] for row in callbacks}))
    result = legacy.audit(symbols, posix_blocks(symbols, functions), roots=roots,
        required_imports=("fstat", "sceKernelFsync"), indirect_roots=indirect,
        all_imports_in_scope=True)
    proof = cpu_proof(source_root, body, symbols)
    direct_cpu = sorted(name for name in result["observed_probe_references"]
        if legacy.canonical_name(symbols.get(name, {}).get("name", stack.base_name(name))) in {"getrusage", "getrusage_time64"})
    if direct_cpu:result["passed"] = False
    result.update(schema="X4_T26_POSIX_AUDIT1", t26_dynamic_cpu_proof=proof,
        forbidden_static_cpu_routes=direct_cpu,
        t26_callback_bindings=[{key: str(value) if isinstance(value, Path) else value for key, value in row.items()} for row in callbacks],
        scope="T26 main/bank/decoder/converters/copy/real callbacks and FILE backends. All ELF imports retain the legacy ENOSYS-name gate.")
    return result

def run(elf, objdump=None, supplied=None, source_root=ROOT):
    body = Path(elf).read_bytes();symbols = stack.elf_symbols(body)
    tool = objdump or shutil.which("objdump") or shutil.which("llvm-objdump")
    if supplied is None and not tool:raise ValueError("Full linked disassembly required")
    text = supplied if supplied is not None else subprocess.check_output([tool, "-drwC", str(elf)], text=True)
    result = audit(symbols, stack.parse_disassembly(text), Path(source_root), body)
    result.update(elf_sha256=hashlib.sha256(body).hexdigest(), elf_bytes=len(body),
        disassembly_sha256=hashlib.sha256(text.encode()).hexdigest())
    return result

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument("elf", type=Path)
    parser.add_argument("--output", type=Path, required=True);parser.add_argument("--objdump")
    parser.add_argument("--disassembly", type=Path);parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        supplied = args.disassembly.read_text(encoding="utf8") if args.disassembly else None
        result = run(args.elf, args.objdump, supplied, args.source_root)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf8")
        print(json.dumps({key: result[key] for key in ("passed", "elf_sha256", "forbidden_imports", "reachable_linked_stubs", "missing_required_native_imports", "forbidden_static_cpu_routes")}))
        return 0 if result["passed"] else 1
    except (ValueError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print("T26 POSIX gate failed: " + str(error), file=sys.stderr);return 2

if __name__ == "__main__":raise SystemExit(main())
