#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror ${CFLAGS:-} -Isrc/media -Isrc/streaming \
    tests/t43c_reorder_nack_replay_test.c src/media/rtp_reorder.c \
    src/streaming/receiver_nack_state.c -o "$test_dir/replay"
"$test_dir/replay"
