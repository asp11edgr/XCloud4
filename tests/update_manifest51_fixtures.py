#!/usr/bin/env python3
"""Create isolated, signed test fixtures. Only disposable test keys are used."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from make_update_manifest import make_manifest


def package(title_id, version, app_ver):
    content_id = f"IV0000-{title_id}_00-XCLOUD4APP000000"
    entries, keys, values = bytearray(), bytearray(), bytearray()
    for name, value in (("TITLE_ID", title_id), ("CONTENT_ID", content_id), ("CATEGORY", "gd"),
                        ("X4_PRODUCT_VERSION", version), ("APP_VER", app_ver)):
        raw = value.encode("ascii") + b"\0"
        entries.extend(struct.pack("<HHIII", len(keys), 0x0204, len(raw), len(raw), len(values)))
        keys.extend(name.encode("ascii") + b"\0")
        values.extend(raw)
    keys_at = 20 + len(entries)
    sfo = (b"\x00PSF\x01\x01\0\0" + struct.pack("<III", keys_at, keys_at + len(keys), 5)
           + entries + keys + values)
    pkg = bytearray(0x140)
    pkg[:4] = b"\x7fCNT"
    struct.pack_into(">III", pkg, 0x10, 1, 0, 0x100)
    pkg[0x40:0x64] = content_id.encode("ascii")
    struct.pack_into(">6I", pkg, 0x100, 0x1000, 0, 0, 0, len(pkg), len(sfo))
    return bytes(pkg) + sfo


def openssl(*args):
    subprocess.run(["openssl", *map(str, args)], check=True, stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--real-pkg", type=Path)
    args = parser.parse_args()
    out = args.output
    out.mkdir(mode=0o700)
    key = out / "PRUEBA-51-DESCARTABLE.pem"
    wrong_key = out / "PRUEBA-51-OTRA-DESCARTABLE.pem"
    for path in (key, wrong_key):
        openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", path)
    openssl("pkey", "-in", key, "-pubout", "-outform", "DER", "-out", out / "public.der")

    fixture = package("XCLD00001", "0.7.100-lab", "01.68")
    (out / "valid.pkg").write_bytes(fixture)
    (out / "wrong_magic.pkg").write_bytes(b"\0" + fixture[1:])
    (out / "other_title.pkg").write_bytes(package("XCLD00003", "0.7.100-lab", "01.68"))
    (out / "wrong_app_ver.pkg").write_bytes(package("XCLD00001", "0.7.100-lab", "01.67"))
    (out / "wrong_version.pkg").write_bytes(package("XCLD00001", "0.7.99-lab", "01.68"))
    valid = make_manifest(out / "valid.pkg")
    assert valid["pkg_size"] == len(fixture) and valid["pkg_sha256"] == hashlib.sha256(fixture).hexdigest()

    def sign(name, manifest, signing_key=key):
        path = out / (name + ".json")
        path.write_bytes((json.dumps(manifest, ensure_ascii=True, indent=2) + "\n").encode("utf-8"))
        openssl("dgst", "-sha256", "-sign", signing_key, "-out", out / (name + ".sig"), path)

    sign("valid", valid)
    sign("wrong_key", valid, wrong_key)
    for name, changes in {
        "edition": {"edition": "public"},
        "title": {"title_id": "XCLD00003"},
        "same_version": {"version": "0.7.99-lab"},
        "older_version": {"version": "0.7.98-lab"},
        "same_app_ver": {"app_ver": "01.67"},
        "older_app_ver": {"app_ver": "01.66"},
        "too_big": {"pkg_size": 64 * 1024 * 1024 + 1},
        "unsafe_name": {"pkg_name": "../valid.pkg"},
        "bad_digest": {"pkg_sha256": "Z" * 64},
        "public_stable": {"edition": "public", "title_id": "XCLD00003", "version": "1.3.0", "app_ver": "01.42"},
        "beta_order": {"edition": "public", "title_id": "XCLD00003", "version": "1.3.0-beta.10", "app_ver": "01.42"},
    }.items():
        sign(name, valid | changes)
    if args.real_pkg:
        sign("real_pkg", make_manifest(args.real_pkg))


if __name__ == "__main__":
    main()
