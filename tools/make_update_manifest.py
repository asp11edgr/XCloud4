#!/usr/bin/env python3
"""Genera el manifiesto de actualización sin firma a partir de un PKG."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct

MAX_PACKAGE = 64 * 1024 * 1024
TITLES = {"XCLD00001": "lab", "XCLD00003": "public"}
CONTENT_ID = re.compile(rb"[A-Z0-9]{6}-([A-Z]{4}[0-9]{5})_00-[A-Z0-9]{16}\x00{12}")
PACKAGE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,123}\.pkg")
APP_VER = re.compile(r"[0-9]{2}\.[0-9]{2}")
VERSION = re.compile(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}(?:-([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*))?")


def read_sfo(blob):
    if len(blob) < 20 or blob[:8] != b"\x00PSF\x01\x01\x00\x00":
        raise ValueError("PARAM.SFO no válido")
    keys, data, count = struct.unpack_from("<III", blob, 8)
    if count > 128 or not 20 + count * 16 <= keys < data < len(blob):
        raise ValueError("Tabla PARAM.SFO fuera del archivo")
    expected = {"CONTENT_ID": 37, "TITLE_ID": 10, "APP_VER": 6,
                "X4_PRODUCT_VERSION": 48, "CATEGORY": 3}
    result = {}
    for index in range(count):
        key, kind, length, capacity, value = struct.unpack_from("<HHIII", blob, 20 + index * 16)
        if keys + key >= data or length > capacity or data + value + capacity > len(blob):
            raise ValueError("Entrada PARAM.SFO fuera del archivo")
        end = blob.find(b"\x00", keys + key, data)
        if end <= keys + key:
            raise ValueError("Nombre PARAM.SFO no válido")
        name = blob[keys + key:end].decode("ascii")
        if name not in expected:
            continue
        if name in result or kind != 0x0204 or not 2 <= length <= expected[name]:
            raise ValueError("Campo de identidad PARAM.SFO no válido")
        raw = blob[data + value:data + value + length]
        if raw[-1] != 0 or any(byte < 32 or byte > 126 for byte in raw[:-1]):
            raise ValueError("Texto de identidad PARAM.SFO no válido")
        result[name] = raw[:-1].decode("ascii")
    if result.keys() != expected.keys() or result["CATEGORY"] != "gd":
        raise ValueError("Faltan campos de identidad de un PKG de aplicación XCloud4")
    return result


def package_identity(stream, size):
    prefix = stream.read(0x70)
    if len(prefix) != 0x70 or prefix[:4] != b"\x7fCNT":
        raise ValueError("Cabecera CNT no válida")
    content = CONTENT_ID.fullmatch(prefix[0x40:0x70])
    if not content:
        raise ValueError("Content ID no válido")
    title_id = content[1].decode("ascii")
    if title_id not in TITLES:
        raise ValueError("Title ID ajeno a XCloud4")
    count = struct.unpack_from(">I", prefix, 0x10)[0]
    table = struct.unpack_from(">I", prefix, 0x18)[0]
    end = table + count * 32
    if not 1 <= count <= 4096 or table < len(prefix) or end > size:
        raise ValueError("Tabla de entradas CNT no válida")
    stream.seek(table)
    sfo = None
    for _ in range(count):
        entry = stream.read(32)
        if len(entry) != 32:
            raise ValueError("Entrada CNT incompleta")
        entry_id, _, flags, key_flags, offset, length = struct.unpack_from(">6I", entry)
        if entry_id == 0x1000:
            if sfo or flags or key_flags or not 20 <= length <= 65536 or offset < end or offset + length > size:
                raise ValueError("Entrada PARAM.SFO no válida")
            sfo = offset, length
    if not sfo:
        raise ValueError("Falta PARAM.SFO")
    stream.seek(sfo[0])
    raw = stream.read(sfo[1])
    if len(raw) != sfo[1]:
        raise ValueError("PARAM.SFO incompleto")
    values = read_sfo(raw)
    if values["TITLE_ID"] != title_id or values["CONTENT_ID"].encode() != content[0][:36]:
        raise ValueError("Identidad distinta entre cabecera CNT y PARAM.SFO")
    app_ver, version = values["APP_VER"], values["X4_PRODUCT_VERSION"]
    version_match = VERSION.fullmatch(version)
    if not APP_VER.fullmatch(app_ver) or not version_match:
        raise ValueError("Versiones inválidas")
    if version_match[1] and any(s.isdigit() and len(s) > 1 and s[0] == "0"
                                for s in version_match[1].split(".")):
        raise ValueError("Versión preliminar no válida")
    return title_id, version, app_ver


def make_manifest(pkg):
    name = pkg.name
    if not PACKAGE_NAME.fullmatch(name):
        raise ValueError("Usa un nombre de archivo ASCII que termine en .pkg")
    with pkg.open("rb") as stream:
        original = os.fstat(stream.fileno())
        if not 0x70 <= original.st_size <= MAX_PACKAGE:
            raise ValueError("Tamaño de PKG fuera de límites")
        title_id, version, app_ver = package_identity(stream, original.st_size)
        stream.seek(0)
        digest = hashlib.sha256()
        hashed = 0
        while block := stream.read(1024 * 1024):
            hashed += len(block)
            if hashed > original.st_size:
                raise ValueError("El PKG cambió durante la lectura")
            digest.update(block)
        current = os.fstat(stream.fileno())
        if hashed != original.st_size or current.st_mtime_ns != original.st_mtime_ns or current.st_ctime_ns != original.st_ctime_ns:
            raise ValueError("El PKG cambió durante la lectura")
    return dict(schema=1, edition=TITLES[title_id], title_id=title_id, version=version,
                app_ver=app_ver, pkg_name=name, pkg_size=hashed, pkg_sha256=digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pkg", type=Path)
    parser.add_argument("--output", type=Path, help="Ruta nueva para el JSON. Por defecto se imprime en pantalla.")
    args = parser.parse_args()
    try:
        text = json.dumps(make_manifest(args.pkg), ensure_ascii=True, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8", newline="\n") as output:
                output.write(text)
        else:
            print(text, end="")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
