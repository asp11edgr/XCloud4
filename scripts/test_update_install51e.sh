#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=$1
fixtures=$(realpath "$2")
signed_pkg=$(realpath "$3")
: "$X4_HOST_MBEDTLS_PREFIX"
: "$OO_PS4_TOOLCHAIN"
mkdir "$out"
out=$(realpath "$out")
tree="$out/host-source"
mkdir -p "$tree/src/core" "$tree/build/update"
cp -a src/update "$tree/src/"
cp src/core/build_identity.h "$tree/src/core/"
ln -s "$PWD/src/auth" "$tree/src/auth"
# Run as the installed edition/version from the existing signed trial.
# Only this host identity changes. The updater sources and signed PKG are unmodified.
sed -E '/^#define X4_PRODUCT_VERSION "0\./s/"[^"]+"/"0.7.99-lab"/;
        s/^#define X4_APP_VERSION ".*"/#define X4_APP_VERSION "01.67"/' \
    src/core/app_identity.h > "$tree/src/core/app_identity.h"
python3 -B tools/embed_update_pubkey.py src/update/release_pubkey.pem "$tree/build/update/pubkey51.h"
echo 'T51E_INSTALL_HOST_IDENTITY current=0.7.99/01.67 signed_target=0.7.100/01.68'

libs=("$X4_HOST_MBEDTLS_PREFIX/lib/libmbedcrypto.a")
for name in everest p256m; do
    path="$X4_HOST_MBEDTLS_PREFIX/lib/lib$name.a"
    [[ ! -f $path ]] || libs+=("$path")
done
source=("$tree/src/update/update_job51.c" "$tree/src/update/update_manifest.c"
    "$tree/src/update/pkg_header.c" "$tree/src/auth/json.c" "$tree/src/update/update_io51.c"
    "$tree/src/update/update_log51.c" "$tree/src/update/diag_file51.c" "$tree/src/update/installer51.c")
wrappers='-Wl,--wrap=open,--wrap=stat,--wrap=fstat,--wrap=lseek,--wrap=fread,--wrap=fwrite,--wrap=fclose,--wrap=fgetc,--wrap=mkdir,--wrap=chmod,--wrap=rename,--wrap=unlink,--wrap=write,--wrap=close,--wrap=x4_update_manifest_verify,--wrap=fstatvfs,--wrap=statvfs,--wrap=statfs'
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66

run_case() {
    local mode=$1 dir status
    shift
    "$CC" $X4_TEST_CFLAGS -std=c23 -D_GNU_SOURCE -DX4_OPENORBIS -DX4_PUBLIC=0 -DX4_VIDEO_PROBE=0 \
        -DX4_TEST_BASE51B=0 -DX4_TEST_BASE51C=0 -DX4_TEST_SPACE51D=1 -DX4_TEST_INSTALL51E=1 \
        -O1 -g -pthread -Wall -Wextra -Werror -Werror=vla -Werror=alloca \
        -I"$tree/src/update" -I"$tree/src/core" -I"$tree/src/auth" \
        -I"$X4_HOST_MBEDTLS_PREFIX/include" -idirafter "$OO_PS4_TOOLCHAIN/include" \
        "$@" "$wrappers" tests/update_ps4_io51c_test.c "${source[@]}" "${libs[@]}" -o "$out/$mode"
    echo "T51E_INSTALL_MODE $mode"
    mkdir "$out/$mode-complete"
    "$out/$mode" "$out/$mode-complete" "$fixtures" "$signed_pkg" complete
    for action in cancel retry stale; do
        dir="$out/$mode-boot-$action"
        mkdir "$dir"
        status=0
        "$out/$mode" "$dir" "$fixtures" "$signed_pkg" crash-register || status=$?
        if [[ $status != 79 ]]; then
            echo "Expected Register crash exit 79, got $status" >&2
            return 1
        fi
        echo "T51E_CRASH_REGISTER_EXIT rc=$status"
        "$out/$mode" "$dir" "$fixtures" "$signed_pkg" "boot-$action"
    done
}
run_case plain
run_case sanitized -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -no-pie
run_case tsan -fsanitize=thread -fno-omit-frame-pointer
