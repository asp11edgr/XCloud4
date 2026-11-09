#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
includes=(-Itests/host_stubs -idirafter "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include")
sources=(tests/t27_settings_test.c src/ui/picture_settings.c src/ui/stream_settings.c
 src/ui/server_settings.c src/ui/access_country.c src/ui/apple_ui.c src/ui/canvas.c
 src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c src/ui/screen.c src/ui/net_hint.c
 third_party/qrcodegen/qrcodegen.c)
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror
 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -ffunction-sections -fdata-sections)
for public in 0 1;do
 mkdir -p "$build_dir/settings-$public"
 binary="$build_dir/settings-$public-test"
 "${CC:-clang}" "${flags[@]}" "${includes[@]}" -DX4_PUBLIC="$public" \
  "-DX4_LAB_GPU_SETTINGS_PATH=\"$build_dir/lab-gpu-$public.bin\"" \
  ${X4_TEST_CFLAGS:-} "${sources[@]}" -Wl,--gc-sections -Wl,--wrap=rename -pthread -lm -o "$binary"
 if [[ -n ${X4_TEST_RENDER_DIR:-} ]];then
  mkdir -p "$X4_TEST_RENDER_DIR/$public"
  "$binary" "$build_dir/settings-$public" "$X4_TEST_RENDER_DIR/$public"
 else
  "$binary" "$build_dir/settings-$public"
 fi
 if [[ $public == 1 ]];then
  if nm -a "$binary" | grep -E 'x4_stream_settings_ui_|x4_gpu_quality_settings_|quality_draft|draw_lab_settings|x4_stream_settings_conversion_';then
   printf '%s\n' 'FAIL technical R3/Lab settings present in Public';exit 1
  fi
  if strings "$binary" | grep -E 'Mailbox \(experimental\)|Deband|CAS \(adaptive RGB\)|Si la GPU pausa';then
   printf '%s\n' 'FAIL Lab-only options present in Public';exit 1
  fi
 fi
 if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]];then
  "${CC:-clang}" "${flags[@]}" "${includes[@]}" -DX4_PUBLIC="$public" \
   "-DX4_LAB_GPU_SETTINGS_PATH=\"$build_dir/lab-gpu-$public.bin\"" \
   -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie "${sources[@]}" \
   -Wl,--gc-sections -Wl,--wrap=rename -pthread -lm -o "$binary-sanitized"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
   "$binary-sanitized" "$build_dir/settings-$public"
 fi
done
printf '%s\n' 'PASS T27 real settings/migration/UI and ASan/UBSan; public technical exclusion'
