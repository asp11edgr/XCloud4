#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Requires the exact libdatachannel source after XCloud4's numeric/NACK overlay.
# This does not download, build native SDK modules or launch a network session.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CXX:-clang++}
vendor_dir=${X4_LIBDATACHANNEL_SOURCE:?Set X4_LIBDATACHANNEL_SOURCE to the pinned/patched source tree}
plog_include=${X4_PLOG_INCLUDE:-$vendor_dir/deps/plog/include}
for path in "$vendor_dir/include/rtc/rtp.hpp" "$vendor_dir/include/rtc/message.hpp" \
    "$vendor_dir/src/rtp.cpp" "$vendor_dir/src/message.cpp" \
    "$vendor_dir/src/mediahandler.cpp" "$plog_include/plog/Log.h"; do
    [[ -f "$path" ]] || { echo "FAIL missing pinned dependency input: $path" >&2; exit 2; }
done
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-receiver-nack-wire.XXXXXX")
cleanup() {
    rm -f -- "$build_dir"/*
    rmdir -- "$build_dir"
}
trap cleanup EXIT
cp -- "$repo_dir/src/streaming/rtc_receive_diag.h" "$build_dir/x4_rtc_receive_diag.h"
common=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    -DX4_OPENORBIS -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0 -ffunction-sections -fdata-sections \
    -I"$build_dir" -I"$repo_dir/src/streaming" -I"$vendor_dir/include" \
    -I"$vendor_dir/include/rtc" -I"$vendor_dir/src" -I"$plog_include" \
    "$repo_dir/tests/receiver_nack_wire_test.cpp" \
    "$repo_dir/src/streaming/rtc_receiver_nack.cpp" \
    "$vendor_dir/src/message.cpp" "$vendor_dir/src/rtp.cpp" \
    "$vendor_dir/src/mediahandler.cpp" -Wl,--gc-sections)
echo "Host receiver NACK wire compiler: $compiler"
"$compiler" --version
"$compiler" "${common[@]}" -o "$build_dir/wire-plain"
"$build_dir/wire-plain"
# A minimal independent probe distinguishes missing compiler/runtime support
# from a real fixture compilation/linking failure. Never label the latter as
# an unavailable sanitizer when the toolchain supports it.
printf '%s\n' 'int main() { return 0; }' >"$build_dir/sanitizer-probe.cpp"
if ! "$compiler" -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$build_dir/sanitizer-probe.cpp" -o "$build_dir/sanitizer-probe" \
    >"$build_dir/sanitizer-probe-build.txt" 2>&1; then
    cat "$build_dir/sanitizer-probe-build.txt"
    echo "UNAVAILABLE ASan/UBSan compiler/runtime probe; not a sanitizer pass"
    exit 0
fi
if ! ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$build_dir/sanitizer-probe" >"$build_dir/sanitizer-probe-run.txt" 2>&1; then
    cat "$build_dir/sanitizer-probe-run.txt"
    echo "UNAVAILABLE ASan/UBSan minimal runtime probe; not a sanitizer pass"
    exit 0
fi
"$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$build_dir/wire-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$build_dir/wire-asan-ubsan"
