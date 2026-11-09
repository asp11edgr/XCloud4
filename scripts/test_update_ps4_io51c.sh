#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
base=$(realpath "$1")
out=$2
fixtures=$(realpath "$3")
signed_pkg=$(realpath "$4")
: "$X4_HOST_MBEDTLS_PREFIX"
: "$OO_PS4_TOOLCHAIN"
mkdir "$out"
out=$(realpath "$out")
[[ -f $base/build/update/pubkey51.h ]]
if [[ ! -f build/update/pubkey51.h ]]; then
    python3 -B tools/embed_update_pubkey.py src/update/release_pubkey.pem build/update/pubkey51.h
fi
libs=("$X4_HOST_MBEDTLS_PREFIX/lib/libmbedcrypto.a")
for name in everest p256m; do
    path="$X4_HOST_MBEDTLS_PREFIX/lib/lib$name.a"
    [[ ! -f $path ]] || libs+=("$path")
done
wrappers='-Wl,--wrap=open,--wrap=stat,--wrap=fstat,--wrap=lseek,--wrap=fread,--wrap=fwrite,--wrap=fclose,--wrap=fgetc,--wrap=mkdir,--wrap=chmod,--wrap=rename,--wrap=unlink,--wrap=x4_update_manifest_verify'
run_case() {
    local tree=$1 old=$2 mode=$3
    shift 3
    local -a source=("$tree/src/update/update_job51.c" "$tree/src/update/update_manifest.c"
        "$tree/src/update/pkg_header.c" "$tree/src/auth/json.c")
    local -a compat=()
    if [[ $old == 0 ]]; then
        source+=("$tree/src/update/update_io51.c" "$tree/src/update/update_log51.c" "$tree/src/update/diag_file51.c")
        if grep -q 'X4Install51Space' "$tree/src/update/installer51.h"; then
            compat=(-DX4_TEST_SPACE_API51D=1)
        fi
        if grep -q 'X4Install51Before' "$tree/src/update/installer51.h"; then
            compat+=(-DX4_TEST_INSTALL_API51E=1)
        fi
    fi
    "$CC" $X4_TEST_CFLAGS -std=c23 -D_GNU_SOURCE -DX4_OPENORBIS -DX4_PUBLIC=0 \
        -DX4_VIDEO_PROBE=0 -DX4_TEST_BASE51B="$old" -O1 -g -pthread \
        -Wall -Wextra -Werror -Werror=vla -Werror=alloca \
        -I"$tree/src/update" -I"$tree/src/core" -I"$tree/src/auth" \
        -I"$X4_HOST_MBEDTLS_PREFIX/include" -idirafter "$OO_PS4_TOOLCHAIN/include" \
        "$@" "${compat[@]}" "$wrappers" tests/update_ps4_io51c_test.c "${source[@]}" \
        "${libs[@]}" -o "$out/$mode"
    mkdir "$out/$mode-files"
    echo "T51C_PS4_IO_MODE $mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        TSAN_OPTIONS=halt_on_error=1:exitcode=66 \
        "$out/$mode" "$out/$mode-files" "$fixtures" "$signed_pkg"
}
run_case "$base" 1 base51b
mkdir "$out/base51b-size-files"
"$out/base51b" "$out/base51b-size-files" "$fixtures" "$signed_pkg" --only-size
run_case "$PWD" 0 plain
run_case "$PWD" 0 sanitized -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -no-pie
run_case "$PWD" 0 tsan -fsanitize=thread -fno-omit-frame-pointer
