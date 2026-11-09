#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -I src
  -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_PROBE_T27=1 -DX4_STREAM_PROFILE=1
  tests/probe_t27_view_test.c src/video/probe_t27_view.c src/ui/font.c)
binary="$build_dir/t27-view"
"${CC:-clang}" "${common[@]}" ${X4_TEST_CFLAGS:-} -o "$binary"
if [[ -n "${X4_TEST_RENDER_DIR:-}" ]];then
  mkdir -p "$X4_TEST_RENDER_DIR"
  "$binary" assets/fonts/noto_sans.x4font "$X4_TEST_RENDER_DIR"
else
  "$binary" assets/fonts/noto_sans.x4font
fi
if [[ "${X4_TEST_SANITIZERS:-1}" == 1 ]];then
  "${CC:-clang}" "${common[@]}" -fsanitize=address,undefined \
    -fno-omit-frame-pointer -no-pie -o "$binary-sanitized"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$binary-sanitized" assets/fonts/noto_sans.x4font
  printf '%s\n' 'PASS T27 view ASan/UBSan'
else
  printf '%s\n' 'NOT RUN T27 view ASan/UBSan (X4_TEST_SANITIZERS=0)'
fi
cat > "$build_dir/off.c" <<'EOF'
#include "video/probe_t27_view.h"
#ifdef X4_T27_VIEW_TITLE
#error T27 view escaped its feature guards
#endif
int main(void) { return 0; }
EOF
for flags in "" "-DX4_VIDEO_PROBE=0 -DX4_PROBE_T27=0" \
  "-DX4_VIDEO_PROBE=0 -DX4_PROBE_T27=1" "-DX4_VIDEO_PROBE=1 -DX4_PROBE_T27=0";do
  # Intentional word splitting: this loop only contains fixed compiler flags.
  "${CC:-clang}" -std=c11 -Wall -Wextra -Werror -I src $flags \
    "$build_dir/off.c" src/video/probe_t27_view.c -o "$build_dir/off"
  if nm -g --defined-only "$build_dir/off" | grep -E 'x4_t27_view';then
    printf '%s\n' 'FAIL T27 view symbol present with feature off';exit 1
  fi
  if strings "$build_dir/off" | grep -E 'Banco de decodificador T27|EJEMPLO HOST|Recursos retenidos: banco detenido';then
    printf '%s\n' 'FAIL T27 view status text present with feature off';exit 1
  fi
done
printf '%s\n' 'PASS T27 view: plain and feature-off symbol/text exclusion'
