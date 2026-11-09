#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare a separate T26 policy; never weakens the legacy probe gate.

Same-build ELF, linked disassembly, .su and source hashes are bound together.
Local callbacks have their actual address-taking constructors checked and are
additional audited roots. Indirect dispatch is attributed per owning function
and remains a visible16KiB assumption unless an exact linked proof resolves it.
This is a reviewable static model, never native runtime stack measurement.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import audit_probe_stack as gate
import prepare_probe_stack_policy as legacy

STACK_BYTES = 256 * 1024
ENTRIES = (("main", "main"), ("x4-t26-bank", "bank_worker"),
    ("x4-stream-decode", "stream_pipeline_decode_worker"),
    ("x4-stream-convert0", "stream_pipeline_convert_worker"),
    ("x4-stream-convert1", "stream_pipeline_convert_worker"),
    ("x4-copy0", "copy_worker"), ("x4-copy1", "copy_worker"), ("x4-copy2", "copy_worker"))
REQUIRED_ROOTS = ("_start", "x4_probe_controller_run", "x4_t26_controller_run",
    "x4_probe_thread_trampoline", "x4_probe_hash_file", "x4_t26_cpu_snapshot_with")
FILE_ROOTS = ("__stdio_read", "__stdio_write", "__stdio_seek", "__stdio_close")
# Source constructor, source unit, exact callback member and linked target.
BINDINGS = (
    ("bank_worker", "probe_t26_controller.c", "now_us", "clock_us"),
    ("bank_worker", "probe_t26_controller.c", "sleep_us", "sleep_us"),
    ("bank_worker", "probe_t26_controller.c", "budget_expired", "expired"),
    ("bank_worker", "probe_t26_controller.c", "before", "before_native"),
    ("bank_worker", "probe_t26_controller.c", "display_open", "display_open"),
    ("bank_worker", "probe_t26_controller.c", "display_close", "display_close"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "allocate", "allocate_v1"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "release", "release_v1"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "load_module", "module_v1"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "resolve", "resolve_v1"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "before_native", "before_v1"),
    ("x4_t26_platform_v1_ops", "probe_t26_platform.c", "clock_us", "clock_v1"),
    ("x4_t26_run_realistic", "probe_t26_realistic.c", "on_decode", "observe_decode"),
    ("x4_t26_run_realistic", "probe_t26_realistic.c", "on_flip", "observe_flip"),
    ("x4_t26_run_realistic", "probe_t26_realistic.c", "before_close", "before_close"),
    ("x4_t26_run_realistic", "probe_t26_realistic.c", "after_close", "after_close"),
)
RUN_FIELDS = {"now_us", "sleep_us", "budget_expired", "before", "display_open", "display_close"}
V1_FIELDS = {"release", "allocate", "resolve", "before_native", "load_module", "clock_us",
    "query", "create", "decode", "flush", "delete_decoder"}
PLATFORM_FIELDS = {"before", "is_loaded", "load_module", "query", "available"}
CONTROL = {"if", "for", "while", "switch", "sizeof", "return", "_Static_assert", "static_assert"}

def c_mask(text):
    """Preserve indices/newlines while removing strings/comments for braces."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)

def source_functions(text):
    masked = c_mask(text)
    result = {}
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', masked):
        name = m[1]
        if name in CONTROL:
            continue
        begin = masked.find('{', m.start())
        depth, end = 1, begin + 1
        while end < len(masked) and depth:
            depth += (masked[end] == '{') - (masked[end] == '}')
            end += 1
        if depth:
            raise ValueError("Unbalanced function source: " + name)
        result[name] = {"body": masked[begin+1:end-1], "original": text[begin+1:end-1],
            "line": text.count('\n', 0, m.start()) + 1}
    return result

def symbols_named(symbols, name):
    return {key: row for key, row in symbols.items()
        if not row.get("undefined") and row.get("name", key) == name}

def linked_name(symbols, functions, name, address=None):
    candidates = [(key, row) for key, row in symbols_named(symbols, name).items()
        if address is None or row["address"] == address]
    if len(candidates) != 1:
        raise ValueError("Missing/ambiguous exact linked function: " + name)
    key, row = candidates[0]
    matches = [label for label, fn in functions.items() if fn["address"] == row["address"]]
    if len(matches) != 1:
        raise ValueError("Missing/ambiguous disassembled body: " + key)
    return matches[0]

def map_ownership(text,symbols):
    """Bind C function ownership to same-link text-input ranges and symbols."""
    ranges=[];rows=[];owner=None
    for line in text.splitlines():
        match=re.match(r'^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\d+\s+(.*?)\s*$',line)
        if not match:continue
        at,size,tail=int(match[1],16),int(match[2],16),match[3]
        section=re.fullmatch(r'(.+\.o):\((\.text[^)]*)\)',tail)
        if section:
            owner=section[1];ranges.append((at,at+size,owner));continue
        if tail.startswith('.') or ':(' in tail:owner=None
        if owner and re.fullmatch(r'[A-Za-z_]\w*',tail):rows.append((at,size,tail,owner))
    result={}
    for at,size,name,obj in rows:
        unit=Path(obj.replace('\\','/')).name.removesuffix('.o')+'.c'
        if not unit.startswith('probe_t26'):continue
        matches=[row for row in symbols.values() if not row.get('undefined') and row['address']==at and row.get('name')==name]
        if not matches or any(row['size']!=size for row in matches):
            raise ValueError('Link-map T26 symbol address/size mismatch: '+name)
        if at in result and result[at]!=unit:raise ValueError('Ambiguous T26 text ownership')
        result[at]=unit
    if not result:raise ValueError('Link map lacks same-ELF T26 function ownership')
    return result

def callback_bindings(symbols, functions, source_root,ownership=None):
    """Source member assignment AND an address-bearing linked constructor.

    No dispatch edge is silently suppressed. These proofs establish the actual
    roots whose frames must be audited; runtime Ops dispatch retains its opaque
    charge. Compiler transformation without an attributable address fails.
    """
    rows = []
    for owner, unit, member, target in BINDINGS:
        path = source_root / "src/video" / unit
        table = source_functions(path.read_text(encoding="utf8"))
        if owner not in table or not re.search(r'\.' + re.escape(member) + r'\s*=\s*' + re.escape(target) + r'\b', table[owner]["original"]):
            raise ValueError("Callback assignment changed: " + owner + "." + member)
        owner_key = linked_name(symbols, functions, owner)
        if ownership is not None and ownership.get(functions[owner_key]['address'])!=unit:
            raise ValueError('Callback constructor link-map/source mismatch: '+owner)
        candidates = []
        for ins in functions[owner_key]["instructions"]:
            match = re.search(r'(?:#\s*)?(?:0x)?([0-9a-fA-F]+)\s+<' + re.escape(target) + r'>', ins["operands"])
            if match and not ins["mnemonic"].startswith(("call", "j")):
                address = int(match[1], 16)
                target_key = linked_name(symbols, functions, target, address)
                if ownership is not None and ownership.get(address)!=unit:
                    raise ValueError('Callback target link-map/source mismatch: '+target)
                candidates.append((ins, target_key))
        if not candidates:
            raise ValueError("No linked callback address proof: " + owner + "." + member + " -> " + target)
        ins, target_key = candidates[0]
        rows.append({"constructor": owner_key, "member": member, "target": target_key,
            "address_taking_site": ins["address"], "instruction": ins["text"],
            "source": path, "source_line": table[owner]["line"],
            "interpretation": "Exact source member and same-ELF callback address; dispatch retains its16KiB assumption."})
    # qsort's own measured comparator is an additional root, not an opaque
    # excuse for missing its body. The libc comparator dispatch remains opaque.
    owner = linked_name(symbols, functions, "x4_t26_samples_stats")
    path = source_root / "src/video/probe_t26_metrics.c"
    if not re.search(r'\bqsort\([^;]*\bascending\s*\)', path.read_text(encoding="utf8")):
        raise ValueError("Metrics qsort comparator source changed")
    for ins in functions[owner]["instructions"]:
        match = re.search(r'(?:0x)?([0-9a-fA-F]+)\s+<ascending>', ins["operands"])
        if match and not ins["mnemonic"].startswith(("call", "j")):
            rows.append({"constructor": owner, "member": "qsort comparator", "target": linked_name(symbols, functions, "ascending", int(match[1], 16)),
                "address_taking_site": ins["address"], "instruction": ins["text"], "source": path,
                "source_line": 1, "interpretation": "Actual comparator address and independent body; libc dispatch remains opaque."})
            break
    else:
        raise ValueError("No same-ELF qsort comparator address proof")
    return rows

def readonly_bytes(body, address, size):
    if not body or len(body)<64 or body[:6]!=b"\x7fELF\x02\x01":raise ValueError("ELF64 readonly-table proof required")
    header=struct.unpack_from("<16sHHIQQQIHHHHHH",body)
    if header[11]<64:raise ValueError("Malformed table-proof sections")
    for i in range(header[12]):
        at=header[6]+i*header[11]
        if at>len(body)-64:raise ValueError("Truncated table-proof sections")
        section=struct.unpack_from("<IIQQQQIIQQ",body,at)
        if section[1]!=1 or not section[2]&2 or section[2]&1:continue
        if section[3]<=address and size<=section[5] and address-section[3]<=section[5]-size:
            offset=section[4]+address-section[3]
            if offset>len(body)-size:raise ValueError("Truncated readonly jump table")
            return body[offset:offset+size]
    raise ValueError("Jump table is not fully contained in readonly allocated ELF data")

def local_jump_table(fn, site, body):
    """Prove the reviewed bounded signed32 PIC-table lowering, no suppression.

    The unchanged stack auditor still charges an opaque16KiB target; this proof
    only explains that the particular computed jmp is an intra-function branch,
    not an unreviewed function-pointer API or recursive call permission.
    """
    if site["kind"]!="tail" or not site["indirect"]:return None
    rows=fn["instructions"]
    at=next((i for i,row in enumerate(rows) if row["address"]==site["address"]),-1)
    if at<3:return None
    lea,load,add,jump=rows[at-3:at+1]
    clean=lambda row:row["operands"].split("#",1)[0].replace(" ","")
    reg=re.fullmatch(r'\*%(r(?:ax|bx|cx|dx|si|di|[89]|1[0-5]))',clean(jump))
    if not reg or add["mnemonic"] not in {"add","addq"}:return None
    target=reg[1];sum_regs=re.fullmatch(r'%(r\w+),%'+target,clean(add))
    if not sum_regs:return None
    base=sum_regs[1]
    indexed=re.fullmatch(r'\(%'+base+r',%(r\w+),4\),%'+target,clean(load))
    if load["mnemonic"] not in {"movslq","movsxd"} or not indexed or lea["mnemonic"] not in {"lea","leaq"}:return None
    if not re.search(r',\s*%'+base+r'\s*#',lea["operands"]):return None
    table=re.search(r'#\s*(?:0x)?([0-9a-fA-F]+)\s+<',lea["operands"])
    if not table:return None
    index=indexed[1]
    aliases={"rax":"eax","rbx":"ebx","rcx":"ecx","rdx":"edx","rsi":"esi","rdi":"edi"}
    index32=aliases.get(index,index+"d")
    bound=None
    for j in range(max(0,at-15),at-3):
        row=rows[j]
        limit=re.fullmatch(r'\$((?:0x)?[0-9a-fA-F]+),%(?:'+index+'|'+index32+r')',clean(row))
        if row["mnemonic"] not in {"cmp","cmpl","cmpq"} or not limit or j+1>=at-3:continue
        branch=rows[j+1]
        if branch["mnemonic"] not in {"ja","jaq"}:continue
        count=int(limit[1],16 if limit[1].startswith("0x") else 10)+1
        if not 1<=count<=64:continue
        # No index rewrite or alternate entry may bypass the checked domain.
        rewritten=False
        for middle in rows[j+2:at-3]:
            if middle["mnemonic"].startswith(("call","j","ret")):rewritten=True;break
            args=clean(middle)
            if re.search(r',%(?:'+index+'|'+index32+r')$',args) and args not in {'%'+index32+',%'+index32}:
                rewritten=True;break
        if not rewritten:bound=(j,count)
    if bound is None:return None
    table_at=int(table[1],16);j,count=bound
    payload=readonly_bytes(body,table_at,count*4)
    targets=[table_at+struct.unpack_from("<i",payload,k*4)[0] for k in range(count)]
    addresses={row["address"] for row in rows}
    if any(target_at not in addresses for target_at in targets):raise ValueError("Computed jump leaves its linked function")
    for row in rows:
        if row["mnemonic"].startswith(("j","loop")) and row is not rows[j+1]:
            branch_at=gate._direct_target(row)
            if branch_at is not None and rows[j]["address"]<branch_at<=site["address"]:
                raise ValueError("Alternate branch bypasses jump-table index bound")
    return {"table_address":table_at,"entries":count,"target_addresses":targets,
        "readonly_sha256":gate.sha256(payload),"bound_instruction":rows[j]["text"],
        "interpretation":"Bounded readonly PIC jump table stays within this function; legacy auditor retains its conservative16KiB unknown-target charge."}

def indirect_reviews(symbols, functions, source_root, elf_body=None,ownership=None):
    """Attribute every T26-owned indirect to reviewed fields in its source TU.

    Inline helpers are followed in the source graph, never permitted merely
    because an indirect instruction exists. New fields/owners fail closed.
    Native function pointers have their typed resolver/source evidence and
    remain16KiB unknown native costs, not measured or invented destinations.
    """
    tables = {}
    for path in sorted((source_root / "src/video").glob("probe_t26*.c")):
        tables[path] = source_functions(path.read_text(encoding="utf8"))
    rows = []
    for linked, fn in functions.items():
        base = fn["name"]
        sites = [site for site in fn["sites"] if site["indirect"]]
        if not sites:
            continue
        owners = [(path, table) for path, table in tables.items() if base in table and
            (ownership is None or ownership.get(fn['address'])==path.name)]
        if not owners:
            continue  # Shared/native bodies stay under the unchanged gate.
        for path, table in owners:
            seen, pending, fields, direct_parameter = set(), [base], set(), False
            while pending:
                name = pending.pop()
                if name in seen:
                    continue
                seen.add(name); body = table[name]["body"]
                fields.update(re.findall(r'(?:->|\.)\s*([A-Za-z_]\w*)\s*\(', body))
                direct_parameter |= name == "x4_t26_cpu_snapshot_with" and bool(re.search(r'\bcall\s*\(', body))
                pending.extend(callee for callee in re.findall(r'\b([A-Za-z_]\w*)\s*\(', body) if callee in table and callee not in seen)
            allowed = RUN_FIELDS if path.name in {"probe_t26_runner.c", "probe_t26_realistic.c"} else V1_FIELDS if path.name == "probe_t26_v1.c" else PLATFORM_FIELDS if path.name == "probe_t26_platform.c" else {"before"} if path.name == "probe_t26_backend.c" else set()
            branch_tables={site["address"]:local_jump_table(fn,site,elf_body) for site in sites}
            if fields - allowed or (not fields and not direct_parameter and any(not branch_tables[site["address"]] for site in sites)):
                raise ValueError("Unreviewed T26 indirect owner/fields: " + linked + " in " + path.name + ": " + repr(sorted(fields)))
            for site in sites:
                rows.append({"function": linked, "address": site["address"], "instruction": site["instruction"],
                    "source": path, "source_line": table[base]["line"], "source_inline_closure": sorted(seen),
                    "reviewed_fields": sorted(fields), "getrusage_parameter": direct_parameter,
                    "local_jump_table": branch_tables[site["address"]],
                    "allowance_bytes": 16384, "proven_runtime_bound": False,
                    "interpretation": "Function-attributed Ops/native dispatch; exact call remains unresolved and charged16KiB by the unchanged stack auditor."})
    return rows

def hash_sites(symbols, functions):
    """Resolve exactly the two SourceOpen hash calls for the T26 cache caller."""
    owner = linked_name(symbols, functions, "x4_t26_clip_load")
    api = linked_name(symbols, functions, "x4_probe_source_open_fully_verified")
    source = linked_name(symbols, functions, "source_open")
    real_hash = linked_name(symbols, functions, "x4_probe_hash_file")
    target_address = functions[real_hash]["address"]
    calls = [site for site in functions[owner]["sites"] if site["target"] == "x4_probe_source_open_fully_verified"]
    if len(calls) != 1:
        raise ValueError("T26 cache must call exactly one fully verified source API")
    call = calls[0];instructions = functions[owner]["instructions"]
    end = next(i for i, row in enumerate(instructions) if row["address"] == call["address"])
    loads = [(i, row) for i, row in enumerate(instructions[:end]) if row["mnemonic"] in ("lea", "leaq") and
        re.search(r',\s*%r9\s*#\s*(?:0x)?' + format(target_address, "x") + r'\s+<x4_probe_hash_file>', row["operands"])]
    if not loads:
        raise ValueError("Missing same-ELF T26 r9 hash argument")
    start, load = loads[-1]
    if any(legacy._writes_register(row, "r9") or row["mnemonic"].startswith(("call", "j", "ret")) for row in instructions[start+1:end]):
        raise ValueError("T26 hash argument overwritten/nonlocal setup")
    for row in instructions:
        if row["mnemonic"].startswith(("j", "loop")):
            target = gate._direct_target(row)
            if target is not None and load["address"] < target <= call["address"]:
                raise ValueError("Branch bypasses T26 hash argument setup")
    wrapper = functions[api]
    if len(wrapper["sites"]) != 1 or wrapper["sites"][0]["kind"] != "tail" or wrapper["sites"][0]["target"] != "source_open" or any(legacy._writes_register(row, "r9") or row["mnemonic"].startswith("call") for row in wrapper["instructions"]):
        raise ValueError("Fully verified wrapper no longer preserves callback argument")
    return [(source, at, real_hash) for at in legacy._prove_callback_register(functions[source], "r15", "r9", 2)]

def prepare(elf_path, su_root, output, source_root=ROOT, disassembly=None, objdump=None,link_map=None):
    elf_path, su_root, output, source_root = map(Path, (elf_path, su_root, output, source_root))
    body = elf_path.read_bytes();symbols = gate.elf_symbols(body)
    map_path=Path(link_map) if link_map is not None else elf_path.with_suffix('.map')
    ownership=map_ownership(map_path.read_text(encoding='utf8'),symbols)
    dump = legacy.read_linked_disassembly(elf_path, disassembly, objdump);functions = gate.parse_disassembly(dump)
    required = {entry for _, entry in ENTRIES} | set(REQUIRED_ROOTS) | set(FILE_ROOTS)
    for name in sorted(required):linked_name(symbols, functions, name)
    header = source_root / "src/video/probe_stack.h"
    if not re.search(r'X4_PROBE_THREAD_STACK_BYTES\s*=\s*256\s*\*\s*1024', header.read_text(encoding="utf8")):
        raise ValueError("T26 configured stack must remain256KiB")
    module = source_root / "src/core/module.c"
    if gate.sha256(module.read_bytes()) != legacy.MODULE_SHA:
        raise ValueError("Shared module recursion source changed; explicit re-review required")
    def evidence(path, detail, kind=None):
        try: artifact = Path(os.path.relpath(Path(path).resolve(), output.parent.resolve())).as_posix()
        except ValueError: artifact = Path(path).resolve().as_posix()
        row = {"artifact": artifact, "sha256": gate.sha256(Path(path).read_bytes()), "detail": detail}
        if kind:row["kind"] = kind
        return row
    configured = evidence(header, "T26 workers use the existing checked native attribute wrapper requesting256KiB; actual console stack/high-water remains unmeasured.", "configured_attribute")
    callbacks = callback_bindings(symbols, functions, source_root,ownership)
    reviews = indirect_reviews(symbols, functions, source_root,body,ownership)
    sites = {f"{owner}@0x{at:x}": {"kind": "indirect_targets", "targets": [target],
        "evidence": evidence(elf_path, "Same-ELF cache hash address in r9, preserving wrapper and every-path r15 provenance prove this exact SourceOpen hash dispatch.")}
        for owner, at, target in hash_sites(symbols, functions)}
    source_cache = source_root / "src/video/probe_t26_cache.c"
    if not re.search(r'x4_probe_source_open_fully_verified\s*\([^;]*\bx4_probe_hash_file\b[^;]*\);', source_cache.read_text(encoding="utf8")):
        raise ValueError("T26 cache no longer supplies the real hash callback")
    usage, empty, own = [], [], set()
    for path in sorted(su_root.rglob("*.su")):
        data = path.read_bytes();records = gate.parse_stack_usage(data.decode("utf8"), str(path))
        item = {"path": Path(os.path.relpath(path.resolve(), output.parent.resolve())).as_posix(), "sha256": hashlib.sha256(data).hexdigest()}
        if not records:empty.append(item);continue
        usage.append(item)
        for name, entries in records.items():
            if any(re.search(r'(?:^|[/\\])src[/\\](?:video[/\\](?:probe_t26|stream_pipeline|live_h264|video_pipeline|decoder_depth|stream_wait)|core[/\\]probe_controller)', record["location"]) for record in entries):own.add(name)
    if not usage:raise ValueError("T26 same-build .su evidence required")
    threads = [{"name": name, "entry": entry, "wrapper_entry": "_start" if name == "main" else "x4_probe_thread_trampoline",
        "stack_bytes": None if name == "main" else STACK_BYTES, "evidence": None if name == "main" else configured,
        "limit_status": "unknown native main stack; no capacity claim" if name == "main" else "checked explicit256KiB attribute; console high-water pending"} for name, entry in ENTRIES]
    module_proof = evidence(module, "Shared loader recursively requests only libSceSysmodule; that name selects no ID branch, so at most two active calls. The legacy source hash is unchanged.")
    proofs = [configured, module_proof, evidence(source_root / "src/video/probe_stack.c", "Native attributes/trampoline source is bound; the exact native imports and .su remain checked by the unchanged stack gate."),
        evidence(map_path,"Same-link T26 input-text ownership is verified against each ELF function address, name and size; local homonyms cannot borrow another source function's review."),
        evidence(source_cache, "The T26 cache is the reviewed fully-verified source caller and supplies real x4_probe_hash_file; linked argument and callee provenance are separately proven.")]
    for row in callbacks:
        row["source_evidence"] = evidence(row.pop("source"), "Callback member assignment and same-ELF address-taking constructor are both checked for " + row["constructor"] + "." + row["member"])
        proofs.append(row["source_evidence"])
    for row in reviews:
        row["source_evidence"] = evidence(row.pop("source"), "Exact owning source function and inline closure attribute this individual unresolved call to reviewed Ops/native fields;16KiB remains an explicit unproved assumption.")
    return {"schema": gate.SCHEMA, "elf_sha256": gate.sha256(body), "max_fraction": .5,
        "individual_frame_limit_bytes": 8192, "opaque_allowance_bytes": 16384, "threads": threads,
        "additional_roots": sorted(set(REQUIRED_ROOTS) | set(FILE_ROOTS) | {row["target"] for row in callbacks}),
        "own_functions": sorted(own), "stack_usage_files": usage, "empty_disabled_translation_units": empty,
        "sites": sites, "required_native_imports": list(legacy.NATIVE_IMPORTS),
        "source_evidence": proofs + [row["source_evidence"] for row in reviews],
        "shared_recursion": [{"members": ["x4_module_open"], "max_visits": 2, "evidence": module_proof}],
        "t26_callback_bindings": callbacks, "t26_indirect_function_reviews": reviews,
        "linked_disassembly_sha256": gate.sha256(dump.encode()),
        "link_map_sha256":gate.sha256(map_path.read_bytes()),
        "hash_callback_disassembly_sha256": gate.sha256(dump.encode()),
        "limitations": ["No legacy gate or global indirect permission is changed.",
            "Linked converter/copy entries are conservatively audited even when the T26 production-GPU path creates no CPU converter/copy workers.",
            "Each unresolved call retains the unchanged16KiB allowance; attribution is not a runtime native stack bound.",
            "Callback bodies are additional roots and real constructor addresses are proved; this does not prove a whole indirect-chain bound.",
            "Exactly the two fully-verified SourceOpen hash dispatches are resolved from the T26 cache call's linked argument provenance.",
            "Every linked own frame/dynamic allocation/recursion remains subject to the8192-byte and50-percent rules.",
            "CPU getrusage is dynamically resolved with typed144-byte LP64 storage; failure is unavailable, never zero CPU.",
            "Main stack capacity and native high-water are console observations, not results of this static model."]}

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True);parser.add_argument("--su-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True);parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--disassembly", type=Path);parser.add_argument("--objdump")
    parser.add_argument("--map",dest="link_map",type=Path)
    args = parser.parse_args(argv)
    try:
        result = prepare(args.elf, args.su_root, args.output, args.source_root, args.disassembly, args.objdump,args.link_map)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf8")
    except (ValueError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print("T26 stack policy failed: " + str(error), file=sys.stderr);return 1
    print("T26 stack policy prepared; explicit opaque assumptions remain unmeasured.");return 0

if __name__ == "__main__":raise SystemExit(main())
