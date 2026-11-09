#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${CC:-clang}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-ui-presentation.XXXXXX")
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT
common=(-std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections -Wl,--gc-sections
    "$repo_dir/tests/catalog_presentation_test.c" "$repo_dir/src/ui/catalog_presentation.c"
    "$repo_dir/src/ui/catalog_browser.c" "$repo_dir/src/ui/recent_games.c"
    "$repo_dir/src/ui/favorites.c" "$repo_dir/src/ui/play_history.c" "$repo_dir/src/ui/product_metadata.c")
"$cc" --version
"$cc" "${common[@]}" -o "$build_dir/plain"; "$build_dir/plain"
printf 'int main(void){return 0;}\n' >"$build_dir/probe.c"
if "$cc" "$build_dir/probe.c" -fsanitize=address,undefined -o "$build_dir/probe" >"$build_dir/probe.log" 2>&1; then
    "$build_dir/probe"
    "$cc" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -o "$build_dir/asan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/asan"
else
    cat "$build_dir/probe.log"; echo 'UNAVAILABLE ASan/UBSan toolchain; not a sanitizer pass'
fi
