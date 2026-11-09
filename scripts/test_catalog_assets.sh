#!/usr/bin/env bash
set -euo pipefail
fixture_root=$(cd "$(dirname "$0")/.." && pwd)
cd "${X4_TEST_SOURCE_ROOT:-$fixture_root}"
baseline=()
native_cache_flags=()
if [[ ${X4_TEST_DISK_NATIVE:-0} == 1 ]]; then
  native_cache_flags=(-DX4_DISK_CACHE_NATIVE_FS_TEST)
fi
if [[ ${X4_ASSETS_TEST_BASELINE31D:-0} == 1 ]]; then
  [[ ${1:-} == --36-cache-diagnostic ]]
  baseline=(-DX4_ASSETS_TEST_BASELINE31D)
fi
if [[ -n ${X4_TEST_OUTPUT_DIR:-} ]]; then
  build_dir=$(mkdir -- "$X4_TEST_OUTPUT_DIR" && cd "$X4_TEST_OUTPUT_DIR" && pwd)
else
  build_dir=$(mktemp -d)
  trap 'rm -rf "$build_dir"' EXIT
fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -DX4_ASSETS_HOST_TEST -O2 -g -Wall -Wextra -Werror \
  ${X4_TEST_CFLAGS:-} "${baseline[@]}" "${native_cache_flags[@]}" -ffunction-sections -fdata-sections -Itests/host_stubs -Isrc \
  -idirafter "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include" \
  "$fixture_root/tests/catalog_assets_test.c" src/ui/screen.c src/ui/catalog_assets.c src/ui/lang.c src/ui/catalog_disk_cache.c src/ui/product_metadata.c \
  src/ui/home_rows.c src/ui/recent_games.c src/ui/catalog_browser.c src/ui/catalog_presentation.c src/ui/image_area.c \
  src/auth/json.c src/auth/public_image_policy.c -Wl,--gc-sections,--wrap=x4_product_surface_decode \
  -Wl,--wrap=x4_catalog_disk_cache_open,--wrap=x4_catalog_disk_cache_get_metadata,--wrap=x4_catalog_disk_cache_get_image \
  -Wl,--wrap=x4_catalog_disk_cache_put_metadata,--wrap=x4_catalog_disk_cache_put_image \
  -pthread -lm -o "$build_dir/catalog-assets-test"
fixtures=("$fixture_root/tests/fixtures/public_product_displaycatalog.json" "$fixture_root/tests/fixtures/ui_synthetic_cover.jpg"
          "$fixture_root/tests/fixtures/ui_synthetic_cover.png" "$fixture_root/tests/fixtures/ui_color_layout.png")
if [[ ${1:-} == --36-suite || ${1:-} == --36-cache-restart ]]; then
  [[ $# == 2 ]]
  if [[ $1 == --36-suite ]]; then "$build_dir/catalog-assets-test" "${fixtures[@]}"; fi
  mkdir -- "$2"
  "$build_dir/catalog-assets-test" "${fixtures[@]}" --36-cache-cold "$2"
  "$build_dir/catalog-assets-test" "${fixtures[@]}" --36-cache-warm "$2"
elif [[ ${1:-} == --40-cache-large ]]; then
  [[ $# == 2 ]]
  mkdir -- "$2"
  "$build_dir/catalog-assets-test" "${fixtures[@]}" --40-cache-seed "$2"
  "$build_dir/catalog-assets-test" "${fixtures[@]}" --40-cache-warm "$2"
elif [[ ${1:-} == --36-cache-diagnostic ]]; then
  [[ $# == 2 ]]
  mkdir -- "$2"
  "$build_dir/catalog-assets-test" "${fixtures[@]}" --36-cache-diagnostic "$2"
else
  "$build_dir/catalog-assets-test" "${fixtures[@]}" "$@"
fi
