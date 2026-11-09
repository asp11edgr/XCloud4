#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${CC:-clang}; cxx=${CXX:-clang++}
vendor=${X4_LIBDATACHANNEL_SOURCE:?Set pinned patched libdatachannel source}
plog=${X4_PLOG_INCLUDE:-$vendor/deps/plog/include}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-receiver43.XXXXXX")
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT
cp -- "$repo_dir/src/streaming/rtc_receive_diag.h" "$build_dir/x4_rtc_receive_diag.h"
c_flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -I"$repo_dir/src/streaming")
cpp_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -DX4_OPENORBIS -DX4_RECOVERY43=1 -DX4_RECOVERY43_HOST_TEST=1 -DX4_PUBLIC=0 -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0
    -I"$build_dir" -I"$repo_dir/src/streaming" -I"$vendor/include" -I"$vendor/include/rtc"
    -I"$vendor/src" -I"$plog")
cpp_sources=("$repo_dir/tests/receiver_chain43_test.cpp" "$repo_dir/src/streaming/rtc_receiver_nack.cpp"
    "$vendor/src/mediahandler.cpp" "$vendor/src/message.cpp" "$vendor/src/rtp.cpp"
    "$vendor/src/rtcpreceivingsession.cpp" "$vendor/src/description.cpp"
    "$vendor/src/impl/utils.cpp")
run() {
    local label=$1; shift
    "$cc" "${c_flags[@]}" "$@" -c "$repo_dir/src/streaming/receiver_nack_state.c" -o "$build_dir/$label-state.o"
    "$cc" "${c_flags[@]}" "$@" -c "$repo_dir/src/streaming/video_repair43.c" -o "$build_dir/$label-repair.o"
    "$cxx" "${cpp_flags[@]}" "$@" "${cpp_sources[@]}" "$build_dir/$label-state.o" \
        "$build_dir/$label-repair.o" -Wl,--gc-sections -o "$build_dir/$label"
    "$build_dir/$label"
}
"$cxx" --version
if [[ ${1:-all} == tsan ]]; then
    TSAN_OPTIONS=halt_on_error=1 run tsan -fsanitize=thread -fno-omit-frame-pointer
else
    run plain
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        run asan -fsanitize=address,undefined -fno-omit-frame-pointer
fi
