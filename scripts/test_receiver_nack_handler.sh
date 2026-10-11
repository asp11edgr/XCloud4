#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Actual generator + pinned MediaHandler. Test doubles replace validation,
# entropy, diagnostics and control bridge; not SRTP/CAPI/inbox/network proof.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CXX:-clang++}
c_compiler=${CC:-clang}
vendor_dir=${X4_LIBDATACHANNEL_SOURCE:?Set X4_LIBDATACHANNEL_SOURCE to the pinned/patched source tree}
plog_include=${X4_PLOG_INCLUDE:-$vendor_dir/deps/plog/include}
for path in "$vendor_dir/include/rtc/message.hpp" "$vendor_dir/src/mediahandler.cpp" \
    "$vendor_dir/src/message.cpp" "$vendor_dir/src/rtp.cpp" "$plog_include/plog/Log.h"; do
    [[ -f "$path" ]] || { echo "FAIL missing pinned fixture input: $path" >&2; exit 2; }
done
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
command -v "$c_compiler" >/dev/null || { echo "FAIL C compiler unavailable: $c_compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-receiver-nack-handler.XXXXXX")
cleanup() { rm -f -- "$build_dir"/*; rmdir -- "$build_dir"; }
trap cleanup EXIT
cp -- "$repo_dir/src/streaming/rtc_receive_diag.h" "$build_dir/x4_rtc_receive_diag.h"
common=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    -DX4_OPENORBIS -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0 -ffunction-sections -fdata-sections \
    -I"$build_dir" -I"$repo_dir/src/streaming" -I"$vendor_dir/include" \
    -I"$vendor_dir/include/rtc" -I"$vendor_dir/src" -I"$plog_include" \
    "$repo_dir/tests/receiver_nack_handler_test.cpp" \
    "$repo_dir/src/streaming/rtc_receiver_nack.cpp" \
    "$vendor_dir/src/mediahandler.cpp" "$vendor_dir/src/message.cpp" "$vendor_dir/src/rtp.cpp" \
    -Wl,--gc-sections)
echo "Host receiver NACK handler compiler: $compiler (C state: $c_compiler)"
"$compiler" --version
"$c_compiler" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -I"$repo_dir/src/streaming" -c "$repo_dir/src/streaming/receiver_nack_state.c" -o "$build_dir/state.o"
"$compiler" "${common[@]}" "$build_dir/state.o" -o "$build_dir/handler-plain"
"$build_dir/handler-plain"
# Probe sanitizer availability independently: if the toolchain supports it,
# any actual fixture build/run failure is a failure, not "unavailable".
printf 'int main(void) { return 0; }\n' >"$build_dir/sanitizer-probe.c"
if "$compiler" -x c++ "$build_dir/sanitizer-probe.c" -fsanitize=address,undefined \
    -o "$build_dir/sanitizer-probe" >"$build_dir/asan-build.txt" 2>&1; then
    "$build_dir/sanitizer-probe"
    "$c_compiler" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$repo_dir/src/streaming" \
    -c "$repo_dir/src/streaming/receiver_nack_state.c" -o "$build_dir/state-asan.o"
    "$compiler" "${common[@]}" "$build_dir/state-asan.o" -fsanitize=address,undefined \
    -fno-omit-frame-pointer -o "$build_dir/handler-asan-ubsan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/handler-asan-ubsan"
else
    cat "$build_dir/asan-build.txt"
    echo "UNAVAILABLE ASan/UBSan host build; not a sanitizer pass"
fi
