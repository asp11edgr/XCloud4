#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Actual host handler/inbox/application ingress/reorder/grace/AU. No SRTP,
# native decoder, network, RGB or console. Test-only stubs stay outside runtime.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${CC:-clang}; cxx=${CXX:-clang++}
vendor=${X4_LIBDATACHANNEL_SOURCE:?Set the pinned patched libdatachannel source tree}
plog=${X4_PLOG_INCLUDE:-$vendor/deps/plog/include}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-nack-pipeline.XXXXXX")
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT
cp -- "$repo_dir/src/streaming/rtc_receive_diag.h" "$build_dir/x4_rtc_receive_diag.h"
c_sources=(tests/receiver_nack_pipeline_worker.c src/streaming/receiver_nack_state.c
    src/streaming/nack_grace_inbox.c src/media/video_ingress.c src/media/rtp_reorder.c
    src/media/nack_grace.c src/media/nack_sequence_binding.c)
c_flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -DX4_INGRESS_HOST_TEST -DX4_GRACE_INBOX_HOST_TEST -DX4_RUMBLE=0
    -I"$repo_dir/tests/host_stubs" -I"$repo_dir/tests" -I"$repo_dir/src/media" -I"$repo_dir/src/streaming")
cpp_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -DX4_OPENORBIS -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0
    -I"$build_dir" -I"$repo_dir/tests" -I"$repo_dir/src/media" -I"$repo_dir/src/streaming"
    -I"$vendor/include" -I"$vendor/include/rtc" -I"$vendor/src" -I"$plog")
cpp_sources=("$repo_dir/tests/receiver_nack_pipeline_test.cpp" "$repo_dir/src/streaming/rtc_receiver_nack.cpp"
    "$vendor/src/mediahandler.cpp" "$vendor/src/message.cpp" "$vendor/src/rtp.cpp")
run_fixture() {
    local label=$1; shift
    local objects=() index=0
    for source in "${c_sources[@]}"; do
        object="$build_dir/$label-$index.o"
        "$cc" "${c_flags[@]}" "$@" -c "$repo_dir/$source" -o "$object"
        objects+=("$object"); index=$((index+1))
    done
    "$cxx" "${cpp_flags[@]}" "$@" "${cpp_sources[@]}" "${objects[@]}" -Wl,--gc-sections -o "$build_dir/$label"
    "$build_dir/$label"
}
"$cc" --version; "$cxx" --version
run_fixture plain
printf 'int main(void){return 0;}\n' >"$build_dir/probe.c"
if "$cxx" -x c++ "$build_dir/probe.c" -fsanitize=address,undefined -o "$build_dir/probe" >"$build_dir/probe.log" 2>&1; then
    "$build_dir/probe"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 run_fixture asan -fsanitize=address,undefined -fno-omit-frame-pointer
else
    cat "$build_dir/probe.log"
    echo 'UNAVAILABLE ASan/UBSan toolchain; not a sanitizer pass'
fi

# Optional private EXACT baseline source root supplied by the build owner.
# Do not publish or create duplicate historical sources in this repository.
# The patched pinned Message adds zero-default metadata only; baseline handler,
# state, inbox, ingress, worker, grace and reorder all come from this archive.
if [[ -n "${X4_BASELINE_SOURCE_DIR:-}" ]]; then
    baseline=$(realpath -- "$X4_BASELINE_SOURCE_DIR")
    [[ -f "$baseline/src/media/live_media.c" && -f "$baseline/src/streaming/rtc_receiver_nack.cpp" ]] || exit 2
    echo "RUN separate exact baseline sources: $baseline (owner verifies archive commit/hash)"
    c_sources=(tests/receiver_nack_pipeline_worker.c)
    baseline_c=(src/streaming/receiver_nack_state.c src/streaming/nack_grace_inbox.c
        src/media/video_ingress.c src/media/rtp_reorder.c src/media/nack_grace.c)
    baseline_objects=(); index=0
    baseline_flags=(-DX4_PIPELINE_BASELINE -DX4_PIPELINE_MEDIA_SOURCE=\"$baseline/src/media/live_media.c\"
        -I"$baseline/src/media" -I"$baseline/src/streaming")
    for source in "${baseline_c[@]}"; do
        object="$build_dir/baseline-$index.o"
        "$cc" "${baseline_flags[@]}" "${c_flags[@]}" -c "$baseline/$source" -o "$object"
        baseline_objects+=("$object"); index=$((index+1))
    done
    "$cc" "${baseline_flags[@]}" "${c_flags[@]}" -c "$repo_dir/tests/receiver_nack_pipeline_worker.c" -o "$build_dir/baseline-worker.o"
    "$cxx" "${baseline_flags[@]}" "${cpp_flags[@]}" "$repo_dir/tests/receiver_nack_pipeline_test.cpp" \
        "$baseline/src/streaming/rtc_receiver_nack.cpp" "$vendor/src/mediahandler.cpp" \
        "$vendor/src/message.cpp" "$vendor/src/rtp.cpp" "${baseline_objects[@]}" \
        "$build_dir/baseline-worker.o" -Wl,--gc-sections -o "$build_dir/baseline-plain"
    "$build_dir/baseline-plain"
else
    echo 'NOT RUN exact baseline source fixture: X4_BASELINE_SOURCE_DIR absent; not a baseline reproduction PASS'
fi
