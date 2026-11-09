#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
  -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs
  tests/stream_gpu_settings_test.c src/ui/stream_settings.c src/ui/canvas.c
  src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c -lm)
for public in 0 1;do
  binary="$build_dir/settings-$public"
  "${CC:-clang}" "${common[@]}" -DX4_PUBLIC="$public" \
    "-DX4_LAB_GPU_SETTINGS_PATH=\"$build_dir/lab-gpu-v1-$public.bin\"" \
    ${X4_TEST_CFLAGS:-} -o "$binary"
  if [[ -n "${X4_TEST_RENDER_DIR:-}" ]];then
    mkdir -p "$X4_TEST_RENDER_DIR"
    "$binary" assets/fonts/noto_sans.x4font "$build_dir/lab-gpu-v1-$public.bin" "$X4_TEST_RENDER_DIR"
  else
    "$binary" assets/fonts/noto_sans.x4font "$build_dir/lab-gpu-v1-$public.bin"
  fi
  if [[ "$public" == 1 ]];then
    if nm -a "$binary" | grep -E 'x4_stream_settings_conversion_|conversion_backend|conversion_checksum|x4_gpu_quality_settings_|quality_settings|quality_draft|draw_lab_settings|x4_stream_settings_ui_|x4_stream_settings_summary|x4_stream_settings_applied_text';then
      printf '%s\n' 'FAIL new Lab GPU settings symbol present in Public';exit 1
    fi
    if strings "$binary" | grep -E 'lab-gpu-v1|Conversion: CPU / GPU|Conversión: CPU / GPU|If GPU submission pauses|Si la GPU pausa|CAS \(adaptive RGB\)|CAS \(RGB adaptativo\)';then
      printf '%s\n' 'FAIL new Lab GPU preference/text present in Public';exit 1
    fi
  fi
  if [[ "${X4_TEST_SANITIZERS:-1}" == 1 ]];then
    "${CC:-clang}" "${common[@]}" -DX4_PUBLIC="$public" \
      "-DX4_LAB_GPU_SETTINGS_PATH=\"$build_dir/lab-gpu-v1-$public.bin\"" \
      -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -o "$binary-sanitized"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      "$binary-sanitized" assets/fonts/noto_sans.x4font "$build_dir/lab-gpu-v1-$public.bin"
  fi
done
printf '%s\n' 'PASS new Lab conversion choice: plain + ASan/UBSan, public symbol/text exclusion'
