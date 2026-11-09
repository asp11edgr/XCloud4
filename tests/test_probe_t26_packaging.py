#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline T26 identity and shell gates; fixtures are not native artifacts."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("probe_packaging_fixtures", ROOT / "tests/test_probe_packaging.py")
fixtures = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixtures)
policy = fixtures.policy
BASE = "b32fd83f01ef28194db445ea73ce4fdd81a2cf78"
SOURCE = hashlib.sha1(("XCloud4-probe-t26b:" +
    "6cdb12365ec29e7ac1ab5169461d1284c38d5dc582efe8ca46c8b63d2405b6aa").encode("utf-8")).hexdigest()


def t26_flags():
    return dict(fixtures.flags(cap=1), X4_PROBE_T26=1, X4_PROBE_T27=0, X4_STREAM_PROFILE=1, X4_PUBLIC=0)


def bash_path():
    found = shutil.which("bash")
    if found:
        return found
    for candidate in (Path("C:/Program Files/Git/bin/bash.exe"), Path("C:/Program Files/Git/usr/bin/bash.exe")):
        if candidate.is_file():
            return str(candidate)
    return None


class T26Packaging(unittest.TestCase):
    def test_probe_t26_metadata_isolated_and_fixed(self):
        identity = policy.metadata("probe", "t26")
        self.assertEqual(identity["title"], "XCloud4 Probe")
        self.assertEqual(identity["title_id"], "XCLD00002")
        self.assertEqual(identity["content_id"], policy.PROBE_CONTENT_ID)
        self.assertEqual(identity["version"], "02.02")
        self.assertEqual(identity["product_version"], "0.7.62-probe.t26b")
        self.assertEqual(identity["stream_profile"], 1)
        self.assertEqual(identity["package_name"], "XCloud4-probe-t26b.pkg")
        self.assertEqual(identity["staging"], "build/package-probe-t26")
        self.assertEqual(identity["dist"], "dist/probe-t26")
        self.assertFalse(identity["include_demo"])
        for mode, variant in (("normal", ""), ("public", ""), ("probe", "off"), ("probe", "on")):
            other = policy.metadata(mode, variant)
            self.assertNotEqual(identity["staging"], other["staging"])
            self.assertNotEqual(identity["dist"], other["dist"])
        for mode, suffix in (("normal", ""), ("public", ""), ("probe", "-perfiles")):
            with self.assertRaises(ValueError):
                policy.metadata(mode, "t26", suffix)

    def test_exact_t26_flags_and_no_cross_identity(self):
        identity, values = policy.metadata("probe", "t26"), t26_flags()
        parsed = policy.parse_config(fixtures.config(values))
        self.assertEqual(parsed, values)
        policy.check_flags(identity, parsed)
        for key, choices in {**policy.FLAG_LIMITS, **policy.OPTIONAL_FLAG_LIMITS}.items():
            for choice in choices:
                if choice == values[key]:
                    continue
                changed = dict(values, **{key: choice})
                with self.assertRaises(ValueError, msg=key):
                    policy.check_flags(identity, changed)
        missing = dict(values)
        del missing["X4_PROBE_T26"]
        with self.assertRaises(ValueError):
            policy.check_flags(identity, missing)
        for mode, variant in (("normal", ""), ("public", ""), ("probe", "off"), ("probe", "on")):
            with self.assertRaises(ValueError):
                policy.check_flags(policy.metadata(mode, variant), values)
        # An SP1 Probe is never treated as the historical off/on variant.
        legacy = fixtures.flags(cap=1)
        legacy["X4_STREAM_PROFILE"] = 1
        with self.assertRaises(ValueError):
            policy.check_flags(policy.metadata("probe", "on"), legacy)
        for unsupported in (-1, 2):
            with self.assertRaises(ValueError):
                policy.parse_config(fixtures.config(dict(values, X4_PROBE_T26=unsupported)))

    def test_t26_source_is_label_hash_and_header_branch_is_exact(self):
        header = (ROOT / "src/core/build_identity.h").read_text(encoding="ascii")
        expected = SOURCE + "-n1-g45-r0-vc1-p1-im-sp1"
        self.assertEqual(SOURCE, "858a20ad1b27ba46d7db0ef97e76d19426a6e5ce")
        self.assertEqual(policy.build_identity(header, t26_flags(), policy.T26_PRODUCT_VERSION), expected)
        self.assertEqual(policy.build_identity(header.replace("\n", "\r\n"), t26_flags(), policy.T26_PRODUCT_VERSION), expected)
        self.assertLessEqual(len(expected), 64)
        for changed in (
            header + '\n#define X4_SOURCE_ID "' + 'a' * 40 + '"\n',
            header + '\n#define X4_PRODUCT_VERSION "0.7.62-probe.t26"\n',
            header.replace("#if X4_VIDEO_PROBE && X4_PROBE_T26", "#if X4_VIDEO_PROBE || X4_PROBE_T26", 1),
            header + header,
        ):
            with self.assertRaises(ValueError):
                policy.build_identity(changed, t26_flags(), policy.T26_PRODUCT_VERSION)
        for changed in (dict(t26_flags(), X4_PUBLIC=1), dict(t26_flags(), X4_VIDEO_PROBE=0)):
            with self.assertRaises(ValueError):
                policy.build_identity(header, changed, policy.T26_PRODUCT_VERSION)
        with self.assertRaises(ValueError):
            policy.build_identity(header, t26_flags())

    def test_normal_and_public_identity_values_preserved_without_optional_stamp(self):
        header = (ROOT / "src/core/build_identity.h").read_text(encoding="ascii")
        for public, source, product, version in (
            (0, BASE, "0.7.63-lab", "01.31"),
            (1, "a7ddc68b9cc346e0d9a91d8459060b6d127c03fa", "1.3.0-beta", "01.08"),
        ):
            values = dict(fixtures.flags(probe=0, cap=1), X4_RUMBLE=1,
                X4_STREAM_PROFILE=1, X4_PUBLIC=public)
            self.assertNotIn("X4_PROBE_T26", policy.parse_config(fixtures.config(values)))
            identity = policy.metadata("public" if public else "normal")
            self.assertEqual(identity["version"], version)
            self.assertEqual(identity["product_version"], product)
            policy.check_flags(identity, values)
            expected = source + "-n1-g45-r1-vc1-p0-im-sp1"
            self.assertEqual(policy.build_identity(header, values, product), expected)
            self.assertEqual(policy.build_identity(header, dict(values, X4_PROBE_T26=0), product), expected)
            with self.assertRaises(ValueError):
                policy.check_flags(identity, dict(values, X4_PROBE_T26=1))

    def test_t26_sfo_and_preflight_fixture(self):
        identity, values = policy.metadata("probe", "t26"), t26_flags()
        self.assertEqual(policy.verify_sfo(fixtures.sfo(identity["sfo"]), identity), identity["sfo"])
        with tempfile.TemporaryDirectory(prefix="x4-t26-package-") as directory:
            directory = Path(directory)
            cfg, header, elf, self_file = (directory / name for name in ("config.h", "identity.h", "fixture.elf", "fixture.self"))
            cfg.write_text(fixtures.config(values), encoding="ascii")
            header.write_bytes((ROOT / "src/core/build_identity.h").read_bytes())
            build_id = policy.build_identity(header.read_text(encoding="ascii"), values, policy.T26_PRODUCT_VERSION)
            marker = b" OFFLINE SYNTHETIC FIXTURE " + build_id.encode() + b" " + policy.T26_PRODUCT_VERSION.encode()
            elf.write_bytes(b"\x7fELF" + marker)
            self_file.write_bytes(bytes.fromhex("4f153d1d") + marker)
            result = policy.preflight(identity, cfg, header, elf, self_file)
            self.assertEqual(result["build_id"], build_id)
            self.assertEqual(result["identity"]["version"], "02.02")
            self_file.write_bytes(bytes.fromhex("4f153d1d") + b" old Lab fixture 0.7.63-lab")
            with self.assertRaises(ValueError):
                policy.preflight(identity, cfg, header, elf, self_file)

    def test_real_shell_gate_before_staging(self):
        bash = bash_path()
        if not bash:
            self.skipTest("bash absent; run this integration fixture on the build host")
        source = ROOT / "scripts/empaquetar.sh"
        subprocess.run([bash, "-n", str(source)], check=True)
        cases = (
            ("probe", "t26", "1", "1", "0", 41),
            ("probe", "t26", "0", "1", "0", 1),
            ("probe", "t26", "1", "0", "0", 1),
            ("probe", "on", "0", "1", "0", 1),
            ("probe", "on", "1", "0", "0", 1),
            ("normal", "", "1", "1", "0", 1),
            ("public", "", "1", "1", "1", 1),
            ("normal", "", "0", "1", "0", 41),
            ("public", "", "0", "1", "1", 41),
        )
        for mode, variant, t26, profile, public, expected in cases:
            with self.subTest(mode=mode, variant=variant, t26=t26, profile=profile):
                with tempfile.TemporaryDirectory(prefix="x4-t26-shell-") as directory:
                    root = Path(directory)
                    (root / "scripts").mkdir()
                    shutil.copy2(source, root / "scripts/empaquetar.sh")
                    (root / "tools").mkdir()
                    make = root / "tools/make"
                    make.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@" > made-self.txt\nexit 41\n', encoding="ascii")
                    make.chmod(0o755)
                    env = {key: value for key, value in os.environ.items() if not key.startswith("X4_")}
                    env.update(OO_PS4_TOOLCHAIN=str(root / "sdk"), X4_PACKAGE_MODE=mode,
                        X4_PROBE_VARIANT=variant, X4_PROBE_T26=t26, X4_STREAM_PROFILE=profile, X4_PUBLIC=public)
                    env["PATH"] = str(root / "tools") + os.pathsep + env.get("PATH", "")
                    completed = subprocess.run([bash, str(root / "scripts/empaquetar.sh")],
                        cwd=root, env=env, text=True, capture_output=True)
                    self.assertEqual(completed.returncode, expected, msg=completed.stderr)
                    marker = root / "made-self.txt"
                    self.assertEqual(marker.exists(), expected == 41)
                    self.assertFalse((root / "build").exists())
                    self.assertFalse((root / "dist").exists())
                    if marker.exists():
                        args = marker.read_text(encoding="ascii").splitlines()
                        if variant == "t26":
                            self.assertIn("X4_PROBE_T26=1", args)
                            self.assertIn("X4_STREAM_PROFILE=1", args)
                            self.assertIn("X4_VIDEO_PROBE=1", args)
                            self.assertIn("X4_RUMBLE=0", args)
                        else:
                            self.assertFalse(any(arg.startswith("X4_PROBE_T26=") for arg in args))


if __name__ == "__main__":
    unittest.main(verbosity=2)
