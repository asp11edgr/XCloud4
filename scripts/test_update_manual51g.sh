#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=$1
pkg=$(realpath "$2")
mkdir "$out"
out=$(realpath "$out")
: "${X4_HOST_MBEDTLS_PREFIX:?}"
: "${OO_PS4_TOOLCHAIN:?}"
python3 -B tools/make_update_manifest.py "$pkg" --output "$out/fixture.json"
# The ephemeral test private key only traverses pipes. The compiled fixture
# substitutes the test public key and never enters any native build.
key_pipe="$out/test-key.pipe"
mkfifo -m 600 "$key_pipe"
openssl pkey -in "$key_pipe" -pubout -out "$out/test-public.pem" &
reader=$!
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:prime256v1 |
    tee "$key_pipe" |
    openssl dgst -sha256 -sign /dev/stdin -out "$out/fixture.sig" "$out/fixture.json"
wait "$reader"
openssl dgst -sha256 -verify "$out/test-public.pem" -signature "$out/fixture.sig" "$out/fixture.json"
cp "$pkg" "$out/conflict.pkg"
python3 -B - "$out/conflict.pkg" <<'PY'
import pathlib, sys
with pathlib.Path(sys.argv[1]).open("r+b") as f:
    f.seek(200)
    byte = f.read(1)
    assert len(byte) == 1
    f.seek(200)
    f.write(bytes([byte[0] ^ 1]))
PY

libraries=("$X4_HOST_MBEDTLS_PREFIX/lib/libmbedcrypto.a")
for lib in everest p256m; do
    candidate="$X4_HOST_MBEDTLS_PREFIX/lib/lib$lib.a"
    [[ ! -f $candidate ]] || libraries+=("$candidate")
done
wrappers='-Wl,--wrap=open,--wrap=stat,--wrap=lstat,--wrap=fstat,--wrap=fread,--wrap=mkdir,--wrap=chmod,--wrap=rename,--wrap=unlink,--wrap=fstatvfs,--wrap=statvfs,--wrap=statfs'
for id in older equal higher; do
    tree="$out/tree-$id"
    mkdir -p "$tree/src/core" "$tree/build/update"
    cp -a src/update "$tree/src/"
    cp src/core/build_identity.h "$tree/src/core/"
    ln -s "$PWD/src/auth" "$tree/src/auth"
    case "$id" in
        older) version=1.2.9-beta; app=01.45 ;;
        equal) version=1.3.0-beta; app=01.46 ;;
        higher) version=1.4.0-beta; app=01.50 ;;
    esac
    sed -e "s/^#define X4_PRODUCT_VERSION \"1\.3\.0-beta\"/#define X4_PRODUCT_VERSION \"$version\"/" \
        -e "s/^#define X4_APP_VERSION \"01\.47\"/#define X4_APP_VERSION \"$app\"/" \
        src/core/app_identity.h > "$tree/src/core/app_identity.h"
    grep -Fqx "#define X4_PRODUCT_VERSION \"$version\"" "$tree/src/core/app_identity.h"
    grep -Fqx "#define X4_APP_VERSION \"$app\"" "$tree/src/core/app_identity.h"
    python3 -B tools/embed_update_pubkey.py "$out/test-public.pem" "$tree/build/update/pubkey51.h"
done

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
for mode in plain sanitized tsan; do
    flags=()
    case "$mode" in
        sanitized) flags=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -no-pie) ;;
        tsan) flags=(-fsanitize=thread -fno-omit-frame-pointer) ;;
    esac
    echo "T51G_MANUAL_MODE $mode"
    for id in older equal higher; do
        tree="$out/tree-$id"
        binary="$out/$mode-$id"
        # Put this one translation unit in its identity tree so its quoted
        # app_identity.h resolves to the same simulated version as production.
        mkdir -p "$tree/tests"
        cp tests/update_manual51g_test.c "$tree/tests/"
        "${CC:-cc}" ${X4_TEST_CFLAGS:-} "${flags[@]}" -std=c23 -D_GNU_SOURCE \
            -DX4_OPENORBIS -DX4_PUBLIC=1 -DX4_VIDEO_PROBE=0 \
            -O1 -g -pthread -Wall -Wextra -Werror -Werror=vla -Werror=alloca \
            -I"$X4_HOST_MBEDTLS_PREFIX/include" -idirafter "$OO_PS4_TOOLCHAIN/include" \
            "$tree/tests/update_manual51g_test.c" "$tree/src/auth/json.c" \
            "$tree/src/update/"{update_job51,update_manifest,pkg_header,update_io51,update_log51,diag_file51,installer51,manual51}.c \
            "$wrappers" "${libraries[@]}" -o "$binary"
        if [[ $id == older ]]; then
            for scenario in move same conflict fallback; do
                dir="$out/$mode-$scenario"
                mkdir -p "$dir/pkg"
                if [[ $scenario == same ]]; then ln "$pkg" "$dir/pkg/${pkg##*/}"; fi
                if [[ $scenario == conflict ]]; then ln "$out/conflict.pkg" "$dir/pkg/${pkg##*/}"; fi
                "$binary" "$dir" "$pkg" "$out/fixture.json" "$out/fixture.sig" "$scenario"
            done
        else
            dir="$out/$mode-applied-$id"
            cp -al "$out/$mode-move" "$dir"
            "$binary" "$dir" "$pkg" "$out/fixture.json" "$out/fixture.sig" applied
        fi
    done
done
