#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc-clean-17/mbedtls}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
test -f "$dep/library/sha256.c" && test -f "$dep/library/platform_util.c" || {
    echo "FAIL real MbedTLS source unavailable: $dep" >&2; exit 2;
}
test -f "$repo/src/video/probe_t26_samples.h" || {
    echo "FAIL generate the real T26 sample type/header before compiling" >&2; exit 2;
}
# The real probe-files test seam intentionally admits only /tmp/ roots.
# A build runner may set TMPDIR inside its checkout; do not weaken that seam.
build=$(mktemp -d "/tmp/x4-probe-t26-cache.XXXXXX")
# Preserve newly generated artifacts and fixtures for review; no work cleanup.
echo "T26_CACHE_TEST_ARTIFACTS $build"
python3 -B "$repo/tests/generate_probe_t26_cache_fixtures.py" --output "$build/fixtures"
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]]; then read -r -a extra <<< "$X4_TEST_CFLAGS"; fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1
    -DX4_STREAM_PROFILE=1 -DX4_PROBE_FILES_TESTING -I"$repo/src/video"
    -I"$build/fixtures" -I"$dep/include")
# The inherited sanitizer flags include -no-pie, which is link-only and
# triggers Clang's unused-command-line warning on the separate -c step.
compile_extra=()
for option in "${extra[@]}"; do
    case "$option" in -no-pie|-pie|-Wl,*) ;; *) compile_extra+=("$option");; esac
done
"$compiler" "${common[@]}" "${compile_extra[@]}" -Dmalloc=x4_t26_cache_test_malloc \
    -Dfree=x4_t26_cache_test_free -Dmemcpy=x4_t26_cache_test_memcpy \
    -c "$repo/src/video/probe_t26_cache.c" -o "$build/cache.o"
"$compiler" "${common[@]}" "${extra[@]}" "$repo/tests/probe_t26_cache_test.c" "$build/cache.o" \
    "$repo/src/video/probe_source.c" "$repo/src/video/probe_files.c" "$repo/src/video/probe_hash.c" \
    "$dep/library/sha256.c" "$dep/library/platform_util.c" \
    -Wl,--wrap=open -Wl,--wrap=x4_probe_hash_context_init -Wl,--wrap=x4_probe_source_next -o "$build/probe-t26-cache-test"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$build/probe-t26-cache-test"
