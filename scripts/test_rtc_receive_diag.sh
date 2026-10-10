#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf -- "$TEMP"' EXIT
CC=${CC:-cc}
flags=(-std=c2x -O2 -g -Wall -Wextra -Werror -DX4_RECEIVE_DIAG_HOST_TEST -pthread -D_DEFAULT_SOURCE)
"$CC" "${flags[@]}" "$ROOT/tests/rtc_receive_diag_test.c" "$ROOT/src/streaming/rtc_receive_diag.c" -o "$TEMP/receive-test"
"$TEMP/receive-test"
if [[ ${X4_DIAG_SANITIZERS:-1} == 1 ]]; then
    for sanitizer in address,undefined thread; do
        if ! "$CC" "${flags[@]}" -O1 -fno-omit-frame-pointer -fsanitize="$sanitizer" \
            "$ROOT/tests/rtc_receive_diag_test.c" "$ROOT/src/streaming/rtc_receive_diag.c" \
            -o "$TEMP/receive-sanitized" >"$TEMP/sanitizer-build.log" 2>&1; then
            cat "$TEMP/sanitizer-build.log"
            if grep -Eq 'cannot find.*(libclang_rt|libasan|libtsan)|unable to find library.*clang_rt|unsupported argument.*fsanitize' "$TEMP/sanitizer-build.log"; then
                printf 'UNAVAILABLE sanitizer %s: compiler/runtime not installed\n' "$sanitizer"
                continue
            fi
            exit 1
        fi
        cat "$TEMP/sanitizer-build.log"
        "$TEMP/receive-sanitized"
    done
fi
