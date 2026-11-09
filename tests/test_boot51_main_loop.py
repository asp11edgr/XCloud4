#!/usr/bin/env python3
"""Execute the actual application main loop with offline PS4/API fixtures."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


TEST = Path(__file__).resolve().with_name("boot51_main_loop_test.c")
SCENARIOS = ("cold", "recent-first", "catalog-first", "preference-mid")
LANGUAGES = ("auto", "es")
UI = (
    "access_country", "apple_ui", "canvas", "catalog_browser", "catalog_gui",
    "catalog_presentation", "favorites", "font", "game_page", "home_rows",
    "image_area", "lang", "loading_screen", "net_hint", "net_hud",
    "picture_settings", "play_history", "product_metadata", "recent_games",
    "screen", "server_settings", "stream_settings",
    "ui_draw_metrics", "ui_lang_pref", "ui_surface", "ui2_account", "ui2_boot",
    "ui2_fortnite", "ui2_home", "ui2_info", "ui2_modal", "ui2_shell", "ui2_theme",
)
WRAPS = ("fopen", "open", "rename", "remove", "unlink", "mkdir", "time",
         "x4_ui_lang_pref_init", "x4_ui2_boot_reset")


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def digest(file):
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build(root, out, public, mode):
    out.mkdir()
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sdk = Path(os.environ["OO_PS4_TOOLCHAIN"])
    sanitizer = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                 if mode == "sanitized" else [])
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
             "-Wall", "-Wextra", "-Werror", "-fno-pie", "-DX4_STREAM_PROFILE=1",
             "-DX4_RUMBLE=1", "-DX4_VIDEO_PROBE=0", f"-DX4_PUBLIC={public}",
             "-I", str(root / "src"), "-I", str(root / "tests/host_stubs"),
             "-idirafter", str(sdk / "include"), *sanitizer]
    if not public and (root / "src/update/update_test.h").is_file():
        flags.append("-DX4_UPDATE_TEST_EXPERIMENT=1")
    boot_diag = (root / "src/update/boot_diag51.c").is_file()
    flags.append(f"-DHOST51_BOOT_DIAG={int(boot_diag)}")
    claims_diag = (root / "src/auth/xsts_claims_diag.h").is_file()
    flags.append(f"-DHOST51_XSTS_DIAG={int(claims_diag)}")
    if boot_diag and (root / "src/update/update_job51.h").is_file():
        flags.append("-DX4_UPDATE51_ENABLED=1")
    real_main = root / "src/core/main.c"
    splash = "sceSystemServiceHideSplashScreen" in real_main.read_text(encoding="utf-8")
    flags.append(f"-DHOST51_SPLASH={int(splash)}")
    main_object = out / "real-main.o"
    run([*compiler, *flags, "-Dmain=x4_boot51_real_main", "-c", str(real_main),
         "-o", str(main_object)], cwd=root)
    sources = [TEST, *(root / f"src/ui/{name}.c" for name in UI)]
    sources += [root / name for name in (
        "src/input/touch_menu.c", "src/streaming/stream_profile.c",
        "src/auth/cloud_time.c", "src/auth/json.c", "src/auth/public_image_policy.c",
        "third_party/qrcodegen/qrcodegen.c")]
    if boot_diag and not public:
        sources += [root / "src/update" / name for name in ("boot_diag51.c", "diag_file51.c", "update_log51.c")]
        update_io = root / "src/update/update_io51.c"
        if update_io.is_file():
            sources.append(update_io)
    run([*compiler, *flags, "-c", *map(str, sources)], cwd=out)
    objects = [main_object, *(out / (source.stem + ".o") for source in sources)]
    assert len(set(objects)) == len(objects)
    wrap_flags = [f"-Wl,--wrap={symbol}" for symbol in WRAPS]
    linked = out / "fixture-link.o"
    run([*compiler, *flags, "-r", "-nostdlib", *map(str, objects),
         "-o", str(linked)], cwd=root)
    # Only unavailable project/PS4 boundaries get a stub. An unmodeled
    # invocation fails loudly rather than manufacturing a success or a value.
    undefined = run(["nm", "-u", str(linked)], text=True, capture_output=True).stdout
    fallbacks = re.findall(r"^\s*U\s+((?:x4_|sce)[A-Za-z0-9_]*)$", undefined, re.M)
    stubs = out / "unexpected-boundaries.c"
    stubs.write_text("#include <stdio.h>\n#include <stdlib.h>\n" + "".join(
        f'void {name}(void) {{ fputs("HOST51 unexpected {name}\\n", stderr); abort(); }}\n'
        for name in fallbacks), encoding="ascii")
    binary = out / "main-loop"
    run([*compiler, *flags, "-no-pie", *map(str, objects), str(stubs), *wrap_flags,
         "-pthread", "-lm", "-o", str(binary)], cwd=root)
    return binary, digest(real_main)


def execute(binary, root, directory, scenario, language, require_success=True, diagnostic=False, retry=False,
            splash_boot_fail=False, splash_api_fail=False):
    directory.mkdir()
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1"}
    if diagnostic:
        env["HOST51_DIAGNOSTIC"] = "1"
    else:
        env.pop("HOST51_DIAGNOSTIC", None)
    if retry:
        env["HOST51_DIAG_RETRY"] = "1"
    else:
        env.pop("HOST51_DIAG_RETRY", None)
    for flag, enabled in (("HOST51_SPLASH_BOOT_FAIL", splash_boot_fail), ("HOST51_SPLASH_API_FAIL", splash_api_fail)):
        if enabled:
            env[flag] = "1"
        else:
            env.pop(flag, None)
    response = subprocess.run([binary, directory, scenario, language], cwd=root,
                              env=env, text=True, capture_output=True, timeout=90)
    (directory / "stdout.log").write_text(response.stdout, encoding="utf-8")
    (directory / "stderr.log").write_text(response.stderr, encoding="utf-8")
    if not require_success:
        print("HOST51_BASELINE", scenario, language, response.returncode,
              response.stderr.strip(), flush=True)
        assert response.returncode != 0 and "requests == 1" in response.stderr, response.stdout
        return {"scenario": scenario, "language": language, "request_missing": True}
    if response.returncode != 0:
        print(response.stdout, response.stderr, flush=True)
    response.check_returncode()
    evidence = [line for line in response.stdout.splitlines() if line.startswith("HOST51_LOOP ")]
    assert len(evidence) == 1, response.stdout
    splash = [line for line in response.stdout.splitlines() if line.startswith("HOST51_SPLASH ")]
    expected_splash = (f"HOST51_SPLASH calls=1 failed_initial_presents={5 if splash_boot_fail else 0} "
                       f"code={-errno.EIO if splash_api_fail else 0}")
    assert splash == [expected_splash], response.stdout
    print(splash[0], flush=True)
    print(evidence[0], flush=True)
    pixels = directory / "last.rgbx"
    assert pixels.stat().st_size == 1920 * 1080 * 4
    checksum = digest(pixels)
    pixels.unlink()  # Temporary frame dump, retain its hash and fixture logs.
    return {"scenario": scenario, "language": language, "pixel_sha256": checksum,
            "measurement": evidence[0], "splash": splash[0]}


def diagnostic_result(directory, public, periodic=True, splash_api_fail=False, claims_diag=False):
    path = directory / "ui-boot-diag.json"
    assert not list(directory.glob("ui-boot-diag.json.*tmp"))
    if public:
        assert not path.exists()
        return {"public": True, "written": False}
    result = json.loads(path.read_bytes())
    expected = {"schema", "sesion_restaurada_ms", "gamertag_conocido_ms",
                "catalogo_peticion_ms", "catalogo_llegada_ms", "recientes_peticion_ms",
                "recientes_llegada_ms", "sigl_peticion_ms", "sigl_llegada_ms",
                "inicio_primer_dibujo_ms", "inicio_ultimo_dibujo_ms", "idioma_carga_ms",
                "dibujos_inicio", "fotogramas_saltados", "splash_oculto_ms", "splash_codigo"}
    if claims_diag:
        expected.update(("xsts_respuestas_omitidas", "xsts_claims"))
    assert set(result) == expected and result["schema"] == 1
    assert all(type(value) is int and 0 <= value < 60000
               for key, value in result.items() if key not in {"splash_codigo", "splash_oculto_ms", "xsts_claims"})
    if claims_diag:
        expected_claims = [{"relying_party": "http://gssv.xboxlive.com/", "http": 200,
                            "nombres_omitidos": 0, "nombres": ["uhs", "gtg"]}]
        if periodic:
            expected_claims.append({"relying_party": "http://mp.microsoft.com/", "http": 200,
                                    "nombres_omitidos": 0, "nombres": ["xid"]})
        assert result["xsts_claims"] == expected_claims and result["xsts_respuestas_omitidas"] == 0
    assert type(result["splash_codigo"]) is int and result["splash_codigo"] == (-errno.EIO if splash_api_fail else 0)
    if splash_api_fail:
        assert result["splash_oculto_ms"] is None
    else:
        assert type(result["splash_oculto_ms"]) is int
        assert 0 <= result["splash_oculto_ms"] <= result["inicio_primer_dibujo_ms"]
    assert result["inicio_ultimo_dibujo_ms"] > result["inicio_primer_dibujo_ms"]
    assert result["catalogo_peticion_ms"] < result["catalogo_llegada_ms"]
    assert result["sigl_peticion_ms"] < result["sigl_llegada_ms"]
    assert result["dibujos_inicio"] >= 3 and result["fotogramas_saltados"] > (500 if periodic else 200)
    return {"public": False, "written": True, "content": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    root, out = args.source.resolve(), args.out.resolve()
    out.mkdir()
    if args.baseline:
        binary, source_hash = build(root, out / "baseline", 0, "plain")
        row = execute(binary, root, out / "baseline-no-request", "recent-first", "auto", False)
        (out / "result.json").write_text(json.dumps({"source_sha256": source_hash,
            "passed": True, "negative_control": row}, indent=2) + "\n", encoding="utf-8")
        return
    rows = []
    diagnostics = []
    splash_cases = []
    claims_diag = (root / "src/auth/xsts_claims_diag.h").is_file()
    editions = (0,) if args.quick else (0, 1)
    modes = ("plain",) if args.quick else ("plain", "sanitized")
    for public in editions:
        for mode in modes:
            case_dir = out / f"public-{public}-{mode}"
            binary, source_hash = build(root, case_dir, public, mode)
            for language in LANGUAGES:
                expected = None
                for scenario in SCENARIOS:
                    row = execute(binary, root, case_dir / (scenario + "-" + language),
                                  scenario, language)
                    if expected is None:
                        expected = row["pixel_sha256"]
                    assert row["pixel_sha256"] == expected, (row, expected)
                    rows.append({**row, "same_as_cold": True, "public": bool(public),
                                 "mode": mode, "source_sha256": source_hash})
            if mode == "plain" and not args.quick:
                diagnostic_dir = case_dir / "boot-diagnostic-auto"
                execute(binary, root, diagnostic_dir, "recent-first", "auto", diagnostic=True)
                diagnostics.append(diagnostic_result(diagnostic_dir, public, claims_diag=claims_diag))
                print("HOST51_DIAGNOSTIC_PASS", "public" if public else "lab", flush=True)
                if (root / "src/update/boot_diag51.c").is_file():
                    diagnostics.append({"exit": True, **diagnostic_result(
                        case_dir / "recent-first-auto", public, periodic=False, claims_diag=claims_diag)})
                    if not public:
                        retry_dir = case_dir / "boot-diagnostic-retry"
                        execute(binary, root, retry_dir, "recent-first", "auto", diagnostic=True, retry=True)
                        diagnostics.append({"retry": True, **diagnostic_result(retry_dir, public, claims_diag=claims_diag)})
                for tag in ("boot-present-fail", "splash-api-fail"):
                    splash_dir = case_dir / tag
                    splash_api_fail = tag == "splash-api-fail"
                    row = execute(binary, root, splash_dir, "recent-first", "auto",
                                  splash_boot_fail=not splash_api_fail, splash_api_fail=splash_api_fail)
                    splash_cases.append({**row, "case": tag, **diagnostic_result(
                        splash_dir, public, periodic=False, splash_api_fail=splash_api_fail, claims_diag=claims_diag)})
    print("HOST51_MAIN_LOOP_PASS", len(rows), "renders; all final pixel hashes match cold",
          flush=True)
    (out / "result.json").write_text(json.dumps({"passed": True, "cases": rows,
                                                "diagnostics": diagnostics, "splash_cases": splash_cases},
                                              indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
