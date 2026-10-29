#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fixed packaging identity and preflight, not a compiler/runtime proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import struct

NORMAL_TITLE_ID = "XCLD00001"
NORMAL_CONTENT_ID = "IV0000-XCLD00001_00-XCLOUD4APP000000"
PROBE_TITLE_ID = "XCLD00002"
PROBE_CONTENT_ID = "IV0000-XCLD00002_00-XCLOUD4PROBE0000"
PUBLIC_TITLE_ID = "XCLD00003"
PUBLIC_CONTENT_ID = "IV0000-XCLD00003_00-XCLOUD4PUBLIC000"
VERSION = "01.26"
PRODUCT_VERSION = "0.7.58-lab"
PUBLIC_VERSION = "01.07"
PUBLIC_PRODUCT_VERSION = "1.2.1-beta"
NORMAL_SUFFIXES = ("", "-ui", "-baseline", "-nack-off", "-nack-on", "-grace-off", "-grace-45", "-grace-45-rumble", "-720p60", "-1080p60", "-perfiles")
STREAM_SUFFIXES = {"-720p60": 1, "-1080p60": 2, "-perfiles": 1}
FLAG_LIMITS = {"X4_RECEIVER_NACK": (0, 1), "X4_NACK_GRACE_MS": (0, 45),
    "X4_RUMBLE": (0, 1), "X4_INGRESS_MODE_BASELINE": (0, 1),
    "X4_VIDEO_CAPACITY_1080": (0, 1), "X4_VIDEO_PROBE": (0, 1),
    "X4_STREAM_PROFILE": (0, 1, 2)}
OPTIONAL_FLAG_LIMITS = {"X4_PUBLIC": (0, 1)}


def validate_ids(title_id, content_id):
    if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", title_id):
        raise ValueError("Invalid fixed homebrew TITLE_ID syntax")
    match = re.fullmatch(r"[A-Z0-9]{6}-([A-Z]{4}[0-9]{5})_00-[A-Z0-9]{16}", content_id)
    if not match or match[1] != title_id:
        raise ValueError("CONTENT_ID components do not match TITLE_ID")


def metadata(mode="normal", variant="", suffix=""):
    version = VERSION
    product_version = PRODUCT_VERSION
    stream_profile = STREAM_SUFFIXES.get(suffix, 0)
    if mode == "normal":
        if variant or suffix not in NORMAL_SUFFIXES:
            raise ValueError("Normal mode has no probe variant; preserve normal suffix policy")
        title, title_id, content_id = "XCloud4 Lab", NORMAL_TITLE_ID, NORMAL_CONTENT_ID
        package_name = f"XCloud4-{PRODUCT_VERSION}{suffix}.pkg"
        stage, dist, demo = "build/package", "dist", True
    elif mode == "public":
        if variant or suffix not in ("", "-publico"):
            raise ValueError("Public mode has no probe variant and only allows empty or -publico suffix")
        title, title_id, content_id = "XCloud4", PUBLIC_TITLE_ID, PUBLIC_CONTENT_ID
        version, product_version = PUBLIC_VERSION, PUBLIC_PRODUCT_VERSION
        package_name = f"XCloud4-{PUBLIC_PRODUCT_VERSION}-publico.pkg"
        stage, dist, demo = "build/package-public", "dist", False
        stream_profile = 1
    elif mode == "probe":
        if variant not in ("off", "on") or suffix:
            raise ValueError("Probe mode requires explicit off/on and no normal suffix")
        title, title_id, content_id = f"XCloud4 Sonda {variant.upper()}", PROBE_TITLE_ID, PROBE_CONTENT_ID
        package_name = f"XCloud4-{PRODUCT_VERSION}-sonda-capacity-{variant}.pkg"
        stage, dist, demo = f"build/package-probe-{variant}", f"dist/probe-{variant}", False
    else:
        raise ValueError("Package mode must be normal, public or probe")
    validate_ids(title_id, content_id)
    return dict(mode=mode, variant=variant, suffix=suffix,
        stream_profile=stream_profile, title=title, title_id=title_id, content_id=content_id,
        version=version, product_version=product_version, package_name=package_name,
        staging=stage, dist=dist, include_demo=demo,
        sfo={"APP_TYPE": 1, "APP_VER": version, "ATTRIBUTE": 0, "CATEGORY": "gd",
            "CONTENT_ID": content_id, "DOWNLOAD_DATA_SIZE": 0, "SYSTEM_VER": 0,
            "TITLE": title, "TITLE_ID": title_id, "VERSION": version})


def parse_config(text):
    if len(text.encode("utf-8")) > 8192:
        raise ValueError("Build config exceeds bounded size")
    flags = {}
    all_limits = {**FLAG_LIMITS, **OPTIONAL_FLAG_LIMITS}
    for line in text.splitlines():
        match = re.fullmatch(r"\s*#define\s+([A-Z0-9_]+)\s+([0-9]+)\s*", line)
        if not match:
            if line.strip():
                raise ValueError("Build config must contain literal flag definitions only")
            continue
        key, value = match[1], int(match[2])
        if key not in all_limits or key in flags or value not in all_limits[key]:
            raise ValueError("Unknown, duplicate or unsupported build flag")
        flags[key] = value
    if not set(FLAG_LIMITS).issubset(set(flags)):
        raise ValueError("Incomplete seven-flag build config")
    if flags["X4_NACK_GRACE_MS"] and not flags["X4_RECEIVER_NACK"]:
        raise ValueError("Grace requires existing NACK ON")
    return flags


def check_flags(identity, flags):
    base = {k: flags[k] for k in FLAG_LIMITS if k in flags}
    if set(base) != set(FLAG_LIMITS):
        raise ValueError("Incomplete seven-flag build config")
    pub = flags.get("X4_PUBLIC", 0)
    if identity["mode"] == "public":
        expected = dict(X4_RECEIVER_NACK=1, X4_NACK_GRACE_MS=45, X4_RUMBLE=1,
            X4_INGRESS_MODE_BASELINE=0, X4_VIDEO_PROBE=0, X4_VIDEO_CAPACITY_1080=1,
            X4_STREAM_PROFILE=1)
        if pub != 1 or base != expected:
            raise ValueError("Public release requires X4_PUBLIC=1, X4_STREAM_PROFILE=1, n1-g45-r1, MPSC and capacity ON")
    elif identity["mode"] == "normal":
        if pub != 0:
            raise ValueError("A public binary must not be packaged with normal lab identity")
        if base["X4_VIDEO_PROBE"]:
            raise ValueError("A probe binary must not be packaged with normal app identity")
        allow_lab_sp1 = identity["suffix"] == "" and base["X4_STREAM_PROFILE"] == 1
        if not allow_lab_sp1 and base["X4_STREAM_PROFILE"] != identity["stream_profile"]:
            raise ValueError("Streaming profile disagrees with the explicit package suffix")
        if base["X4_STREAM_PROFILE"]:
            expected = dict(X4_RECEIVER_NACK=1, X4_NACK_GRACE_MS=45, X4_RUMBLE=1,
                X4_INGRESS_MODE_BASELINE=0, X4_VIDEO_PROBE=0, X4_VIDEO_CAPACITY_1080=1,
                X4_STREAM_PROFILE=base["X4_STREAM_PROFILE"])
            if base != expected:
                raise ValueError("Streaming experiment requires n1-g45-r1, MPSC and capacity ON")
    else:
        expected = dict(X4_RECEIVER_NACK=1, X4_NACK_GRACE_MS=45, X4_RUMBLE=0,
            X4_INGRESS_MODE_BASELINE=0, X4_VIDEO_PROBE=1,
            X4_VIDEO_CAPACITY_1080=int(identity["variant"] == "on"), X4_STREAM_PROFILE=0)
        if pub != 0 or base != expected:
            raise ValueError("Probe variant disagrees with the exact approved seven flags")


def build_identity(header, flags, expected_product=PRODUCT_VERSION):
    header = header.replace("\r\n", "\n")
    source = re.findall(r'^#define X4_SOURCE_ID "([a-f0-9]{40})"$', header, re.M)
    product = re.findall(r'^#define X4_PRODUCT_VERSION "([0-9]+\.[0-9]+\.[0-9]+(?:-[a-z0-9.]+)?)"$', header, re.M)
    if len(source) != 1 or expected_product not in product:
        raise ValueError("Unambiguous source/product identity required")
    return source[0] + f'-n{flags["X4_RECEIVER_NACK"]}-g{flags["X4_NACK_GRACE_MS"]}' + \
        f'-r{flags["X4_RUMBLE"]}-vc{flags["X4_VIDEO_CAPACITY_1080"]}-p{flags["X4_VIDEO_PROBE"]}' + \
        ("-ib" if flags["X4_INGRESS_MODE_BASELINE"] else "-im") + f'-sp{flags["X4_STREAM_PROFILE"]}'


def bounded_file(path, limit):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError("Missing or oversized preflight input: " + str(path))
    return path.read_bytes()


def preflight(identity, config, source_header, elf, self_file):
    flags = parse_config(bounded_file(config, 8192).decode("ascii"))
    check_flags(identity, flags)
    expected_product = identity.get("product_version", PRODUCT_VERSION)
    build_id = build_identity(bounded_file(source_header, 16384).decode("ascii"), flags, expected_product)
    elf_bytes, self_bytes = bounded_file(elf, 64 * 1024 * 1024), bounded_file(self_file, 64 * 1024 * 1024)
    if not elf_bytes.startswith(b"\x7fELF") or not self_bytes.startswith(bytes.fromhex("4f153d1d")):
        raise ValueError("ELF and SELF are distinct required formats")
    for label, data in (("ELF", elf_bytes), ("SELF", self_bytes)):
        if build_id.encode() not in data or expected_product.encode() not in data:
            raise ValueError(label + " does not carry the selected source/flag identity")
    return dict(identity=identity, flags=flags, build_id=build_id,
        elf={"bytes": len(elf_bytes), "sha256": hashlib.sha256(elf_bytes).hexdigest()},
        self={"bytes": len(self_bytes), "sha256": hashlib.sha256(self_bytes).hexdigest()},
        scope="Identity/config preflight only; object/functional proof and native format validation are separate")


def verify_sfo(data, identity):
    if not isinstance(data, bytes) or len(data) < 20 or len(data) > 65536 or data[:4] != b"\x00PSF":
        raise ValueError("Invalid bounded SFO format")
    key_base, data_base, count = struct.unpack_from("<III", data, 8)
    if count > 128 or 20 + count * 16 > key_base or key_base >= data_base or data_base >= len(data):
        raise ValueError("Invalid SFO table boundaries")
    fields = {}
    for i in range(count):
        key_off, kind, length, capacity, data_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        start = key_base + key_off
        if start >= data_base or length > capacity or data_base + data_off + capacity > len(data):
            raise ValueError("Invalid SFO field extent")
        try:
            end = data.index(b"\0", start, data_base)
            key = data[start:end].decode("ascii")
        except (ValueError, UnicodeError) as error:
            raise ValueError("Invalid SFO key") from error
        if not key or key in fields:
            raise ValueError("Duplicate or missing SFO key")
        raw = data[data_base + data_off:data_base + data_off + length]
        if kind == 0x0204:
            if not raw.endswith(b"\0"):
                raise ValueError("Unterminated SFO text")
            text = raw.rstrip(b"\0")
            if b"\0" in text: raise ValueError("Embedded NUL in SFO text")
            fields[key] = text.decode("utf-8")
        elif kind == 0x0404 and length == 4:
            fields[key] = struct.unpack("<I", raw)[0]
        else:
            raise ValueError("Unsupported SFO field type")
    if any(fields.get(k) != v for k, v in identity["sfo"].items()):
        raise ValueError("Extracted SFO does not match selected package identity")
    validate_ids(fields["TITLE_ID"], fields["CONTENT_ID"])
    return fields

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("metadata", "verify", "sfo"))
    parser.add_argument("--mode", choices=("normal", "public", "probe"), default="normal")
    parser.add_argument("--variant", default="")
    parser.add_argument("--suffix", default="")
    parser.add_argument("--format", choices=("json", "lines"), default="json")
    parser.add_argument("--config", default="build/build_config.h")
    parser.add_argument("--source-header", default="src/core/build_identity.h")
    parser.add_argument("--elf", default="build/xcloud4.elf")
    parser.add_argument("--self", dest="self_file", default="build/eboot.bin")
    parser.add_argument("--sfo-file")
    args = parser.parse_args(argv)
    try:
        identity = metadata(args.mode, args.variant, args.suffix)
        result = preflight(identity, args.config, args.source_header, args.elf, args.self_file) if args.command == "verify" else {"identity": identity, "scope": "Metadata policy only; not an artifact validation"}
        if args.command == "sfo":
            if not args.sfo_file: raise ValueError("Extracted SFO path required")
            result["sfo_fields"] = verify_sfo(bounded_file(args.sfo_file, 65536), identity)
            result["scope"] = "Extracted SFO semantic validation only"
        if args.format == "json":
            print(json.dumps(result, indent=2))
        else:
            fields = (identity["title"], identity["title_id"], identity["content_id"], identity["version"],
                identity["product_version"], identity["package_name"], identity["staging"], identity["dist"],
                "1" if identity["include_demo"] else "0")
            print("\n".join(fields))
    except (ValueError, OSError, UnicodeError) as error:
        print("Package preflight rejected: " + str(error), file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
