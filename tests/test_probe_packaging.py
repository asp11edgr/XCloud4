#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline policy/format fixtures; no actual package, native or console proof."""
import importlib.util
import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package_identity", ROOT / "scripts/package_identity.py")
policy = importlib.util.module_from_spec(spec); spec.loader.exec_module(policy)


def flags(probe=1, cap=0):
    return dict(X4_RECEIVER_NACK=1, X4_NACK_GRACE_MS=45, X4_RUMBLE=0,
        X4_INGRESS_MODE_BASELINE=0, X4_VIDEO_CAPACITY_1080=cap, X4_VIDEO_PROBE=probe, X4_STREAM_PROFILE=0)


def config(values):
    return "".join(f"#define {k} {v}\n" for k, v in values.items())


def sfo(values):
    keys = bytearray(); payload = bytearray(); entries = []
    for key, value in values.items():
        off = len(keys); keys.extend(key.encode("ascii") + b"\0")
        raw, kind = (struct.pack("<I", value), 0x0404) if isinstance(value, int) else (value.encode("utf-8") + b"\0", 0x0204)
        entries.append(struct.pack("<HHIII", off, kind, len(raw), len(raw), len(payload)))
        payload.extend(raw)
    key_base = 20 + 16 * len(entries); data_base = key_base + len(keys)
    return b"\0PSF" + struct.pack("<IIII", 0x101, key_base, data_base, len(entries)) + b"".join(entries) + keys + payload


class Packaging(unittest.TestCase):
    def test_normal_metadata_preserved(self):
        m = policy.metadata()
        self.assertEqual(m["title"], "XCloud4 Lab")
        self.assertEqual(m["title_id"], "XCLD00001")
        self.assertEqual(m["content_id"], "IV0000-XCLD00001_00-XCLOUD4APP000000")
        self.assertEqual(m["version"], "01.26")
        self.assertEqual(m["product_version"], "0.7.58-lab")
        self.assertEqual(m["package_name"], "XCloud4-0.7.58-lab.pkg")
        self.assertTrue(m["include_demo"])
        for suffix in policy.NORMAL_SUFFIXES:
            self.assertEqual(policy.metadata(suffix=suffix)["package_name"], "XCloud4-0.7.58-lab" + suffix + ".pkg")
        with self.assertRaises(ValueError): policy.metadata(suffix="-probe")
        with self.assertRaises(ValueError): policy.metadata(variant="off")

    def test_probe_identity_and_separate_paths(self):
        normal = policy.metadata(); off = policy.metadata("probe", "off"); on = policy.metadata("probe", "on")
        self.assertEqual(off["title"], "XCloud4 Sonda OFF")
        self.assertEqual(on["title"], "XCloud4 Sonda ON")
        self.assertEqual(off["title_id"], on["title_id"])
        self.assertEqual(off["content_id"], on["content_id"])
        self.assertNotEqual(normal["title_id"], off["title_id"])
        self.assertNotEqual(normal["content_id"], off["content_id"])
        self.assertNotEqual(off["staging"], on["staging"])
        self.assertNotEqual(normal["staging"], off["staging"])
        self.assertNotEqual(normal["dist"], off["dist"])
        self.assertFalse(off["include_demo"])
        for mode, variant, suffix in (("bad", "", ""), ("probe", "", ""), ("probe", "ON", ""), ("probe", "off", "-ui")):
            with self.assertRaises(ValueError): policy.metadata(mode, variant, suffix)

    def test_hyphenated_suffix_uses_unambiguous_cli_argument(self):
        # A separate '--suffix', '-perfiles' is parsed as another option.
        # Exercise the real CLI, and require the shell packager to use the
        # same equals form at all three preflight/SELF/SFO call sites.
        text = (ROOT / "scripts/empaquetar.sh").read_text()
        self.assertEqual(text.count('--suffix="$PACKAGE_SUFFIX"'), 3)
        self.assertNotIn('--suffix "$PACKAGE_SUFFIX"', text)
        for suffix in policy.NORMAL_SUFFIXES:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(policy.main(["metadata", "--suffix=" + suffix]), 0)
            self.assertIn('"package_name": "XCloud4-0.7.58-lab' + suffix + '.pkg"', output.getvalue())

    def test_stream_profiles_require_exact_flags_and_suffix(self):
        for profile, suffix in ((1, "-720p60"), (2, "-1080p60")):
            identity = policy.metadata(suffix=suffix)
            values = flags(probe=0, cap=1)
            values["X4_RUMBLE"] = 1
            values["X4_STREAM_PROFILE"] = profile
            policy.check_flags(identity, policy.parse_config(config(values)))
            self.assertEqual(identity["stream_profile"], profile)
            self.assertEqual(identity["title_id"], "XCLD00001")
            for key, allowed in policy.FLAG_LIMITS.items():
                for alternative in allowed:
                    if alternative == values[key]: continue
                    changed = dict(values); changed[key] = alternative
                    with self.assertRaises(ValueError): policy.check_flags(identity, changed)
            if profile == 1:
                # The published Lab already permits its default SP1 build
                # without a historical experiment suffix.
                policy.check_flags(policy.metadata(), values)
            else:
                with self.assertRaises(ValueError): policy.check_flags(policy.metadata(), values)
            wrong_suffix = "-1080p60" if profile == 1 else "-720p60"
            with self.assertRaises(ValueError): policy.check_flags(policy.metadata(suffix=wrong_suffix), values)
            with self.assertRaises(ValueError): policy.check_flags(policy.metadata("probe", "on"), values)
            header = '#define X4_SOURCE_ID "' + 'a' * 40 + '"\n#define X4_PRODUCT_VERSION "0.7.58-lab"\n'
            self.assertTrue(policy.build_identity(header, values).endswith("-sp" + str(profile)))

    def test_public_task22_identity(self):
        m = policy.metadata("public")
        self.assertEqual(m["title"], "XCloud4")
        self.assertEqual(m["title_id"], "XCLD00003")
        self.assertEqual(m["version"], "01.07")
        self.assertEqual(m["product_version"], "1.2.1-beta")
        self.assertEqual(m["package_name"], "XCloud4-1.2.1-beta-publico.pkg")
        self.assertFalse(m["include_demo"])

    def test_identity_components_not_just_length(self):
        policy.validate_ids("XCLD00002", policy.PROBE_CONTENT_ID)
        for title, content in (("XCLD00001", policy.PROBE_CONTENT_ID), ("xcld00002", policy.PROBE_CONTENT_ID),
            ("XCLD00002", "IV0000-XCLD00002_00-XCLOUD4PROBE000/"), ("XCLD00002", policy.PROBE_CONTENT_ID.replace("_00-", "-00_"))):
            with self.assertRaises(ValueError): policy.validate_ids(title, content)

    def test_all_flags_and_default_normal_guard(self):
        for cap, variant in ((0, "off"), (1, "on")):
            values = flags(cap=cap); parsed = policy.parse_config(config(values))
            policy.check_flags(policy.metadata("probe", variant), parsed)
            with self.assertRaises(ValueError): policy.check_flags(policy.metadata(), parsed)
            with self.assertRaises(ValueError): policy.check_flags(policy.metadata("probe", "on" if cap == 0 else "off"), parsed)
            for key, allowed in policy.FLAG_LIMITS.items():
                changed = dict(values); changed[key] = next(v for v in allowed if v != values[key])
                with self.assertRaises(ValueError): policy.check_flags(policy.metadata("probe", variant), changed)
        policy.check_flags(policy.metadata(), policy.parse_config(config(flags(probe=0))))
        for text in (config(flags()).replace(" 1\n", " -1\n", 1), config(flags()) + "#define X4_VIDEO_PROBE 1\n",
            config(flags()).splitlines()[0], config(flags()) + "#define UNKNOWN 0\n", "x" * 8193):
            with self.assertRaises(ValueError): policy.parse_config(text)

    def test_preflight_fixture_chain_and_mismatch(self):
        with tempfile.TemporaryDirectory(prefix="x4-package-policy-") as directory:
            d = Path(directory); cfg = d / "config.h"; header = d / "identity.h"; elf = d / "fixture.elf"; self_file = d / "fixture.self"
            source = "a" * 40
            header.write_bytes(f'#define X4_SOURCE_ID "{source}"\r\n#define X4_PRODUCT_VERSION "0.7.58-lab"\r\n'.encode())
            for cap, variant in ((0, "off"), (1, "on")):
                values = flags(cap=cap); cfg.write_text(config(values)); identity = policy.metadata("probe", variant)
                build_id = policy.build_identity(header.read_bytes().decode(), values)
                # Deliberately synthetic format/identity fixture, NOT a valid ELF/SELF.
                elf.write_bytes(b"\x7fELF OFFLINE FIXTURE " + build_id.encode() + b" 0.7.58-lab")
                self_file.write_bytes(bytes.fromhex("4f153d1d") + b" OFFLINE FIXTURE " + build_id.encode() + b" 0.7.58-lab")
                result = policy.preflight(identity, cfg, header, elf, self_file)
                self.assertNotEqual(result["elf"]["sha256"], result["self"]["sha256"])
                self.assertEqual(result["flags"], values)
                self_file.write_bytes(b"\x7fELF" + build_id.encode() + b" 0.7.58-lab")
                with self.assertRaises(ValueError): policy.preflight(identity, cfg, header, elf, self_file)
                self_file.write_bytes(bytes.fromhex("4f153d1d") + b"wrong-source 0.7.58-lab")
                with self.assertRaises(ValueError): policy.preflight(identity, cfg, header, elf, self_file)

    def test_sfo_semantics_and_corruption(self):
        for identity in (policy.metadata(), policy.metadata("probe", "off"), policy.metadata("probe", "on")):
            raw = sfo(identity["sfo"])
            self.assertEqual(policy.verify_sfo(raw, identity), identity["sfo"])
            for bad in (raw[:15], b"X" + raw[1:], raw[:-1]):
                with self.assertRaises(ValueError): policy.verify_sfo(bad, identity)
            values = dict(identity["sfo"]); values["TITLE_ID"] = "XCLD99999"
            with self.assertRaises(ValueError): policy.verify_sfo(sfo(values), identity)
            values = dict(identity["sfo"]); values["TITLE"] = "Wrong visible title"
            with self.assertRaises(ValueError): policy.verify_sfo(sfo(values), identity)

    def test_packager_uses_fixed_preflight_and_preserved_xml(self):
        text = (ROOT / "scripts/empaquetar.sh").read_text()
        self.assertIn("PACKAGE_MODE=${X4_PACKAGE_MODE:-normal}", text)
        self.assertIn("scripts/package_identity.py verify", text)
        self.assertNotIn("eval ", text)
        self.assertIn('cd "$PACKAGE_ROOT"', text)
        self.assertIn('"$DIST/$PACKAGE_NAME"', text)
        self.assertIn("PACKAGE_FILES='eboot.bin assets/sample.h264", text)
        self.assertNotIn("/data/xcloud4-connection", text)
        self.assertNotRegex(text.lower(), r"(?m)^\s*(ftp|curl|wget|python.*ftplib)\s")

    def test_staged_self_and_sfo_gates_precede_build(self):
        text = (ROOT / "scripts/empaquetar.sh").read_text()
        copied = text.index('cp build/eboot.bin "$PACKAGE_ROOT/eboot.bin"')
        staged_verify = text.index('python3 scripts/package_identity.py verify', copied)
        sfo_end = text.index('entry VERSION Utf8 8 "$VERSION"')
        semantic_verify = text.index('python3 scripts/package_identity.py sfo', sfo_end)
        gp4_build = text.index('"$TOOLS/create-gp4"')
        pkg_build = text.index('"$TOOLS/PkgTool.Core" pkg_build')
        self.assertLess(copied, staged_verify)
        self.assertLess(staged_verify, sfo_end)
        self.assertLess(sfo_end, semantic_verify)
        self.assertLess(semantic_verify, gp4_build)
        self.assertLess(gp4_build, pkg_build)
        self.assertIn('--self "$PACKAGE_ROOT/eboot.bin"', text[staged_verify:sfo_end])
        self.assertIn('--sfo-file "$SFO"', text[semantic_verify:gp4_build])
        for block in (text[staged_verify:sfo_end], text[semantic_verify:gp4_build]):
            self.assertIn('--mode "$PACKAGE_MODE"', block)
            self.assertIn('--variant "$PROBE_VARIANT"', block)
            self.assertIn('--suffix="$PACKAGE_SUFFIX"', block)
        self.assertIn('set -Eeuo pipefail', text)

    def test_staged_artifact_cli_accepts_and_rejects(self):
        with tempfile.TemporaryDirectory(prefix="x4-staged-package-policy-") as directory:
            d = Path(directory)
            cfg, header, elf = d / "config.h", d / "identity.h", d / "fixture.elf"
            copied_self, copied_sfo = d / "eboot.bin", d / "param.sfo"
            values = flags()
            cfg.write_text(config(values))
            header.write_text('#define X4_SOURCE_ID "' + 'a' * 40 + '"\n#define X4_PRODUCT_VERSION "0.7.58-lab"\n')
            build_id = policy.build_identity(header.read_text(), values)
            elf.write_bytes(b"\x7fELF OFFLINE FIXTURE " + build_id.encode() + b" 0.7.58-lab")
            copied_self.write_bytes(bytes.fromhex("4f153d1d") + b" OFFLINE FIXTURE " + build_id.encode() + b" 0.7.58-lab")
            copied_sfo.write_bytes(sfo(policy.metadata("probe", "off")["sfo"]))
            verify = ["verify", "--mode", "probe", "--variant", "off", "--config", str(cfg),
                "--source-header", str(header), "--elf", str(elf), "--self", str(copied_self)]
            semantic = ["sfo", "--mode", "probe", "--variant", "off", "--sfo-file", str(copied_sfo)]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(policy.main(verify), 0)
                self.assertEqual(policy.main(semantic), 0)
                copied_self.write_bytes(bytes.fromhex("4f153d1d") + b" wrong build identity")
                self.assertEqual(policy.main(verify), 1)
                bad = dict(policy.metadata("probe", "off")["sfo"]); bad["TITLE_ID"] = "XCLD00001"
                copied_sfo.write_bytes(sfo(bad))
                self.assertEqual(policy.main(semantic), 1)

if __name__ == "__main__":
    unittest.main(verbosity=2)
