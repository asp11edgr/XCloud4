#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
base=$(realpath "${1:?Pass the verified T40b source directory}")
include=$(realpath "${2:?Pass the libopus 1.5.2 include directory}")
libopus=$(realpath "${3:?Pass the same-version host-built libopus.a}")
test -f "$base/src/audio/live_audio.c" && test -f "$libopus"
tmp=$(mktemp -d)
trap 'rm -f -- "$tmp/include/opus" "$tmp"/{base,new}/{test,log,no_loss.pcm,no_fec_5pct.pcm}; rmdir -- "$tmp"/{include,base,new} "$tmp"' EXIT
mkdir -- "$tmp"/{include,base,new}
if [[ -f $include/opus/opus.h ]]; then include="$include/opus"; fi
test -f "$include/opus.h"
ln -s -- "$include" "$tmp/include/opus"
for variant in base new; do
    tree=$PWD
    baseline=0
    if [[ $variant == base ]]; then tree=$base; baseline=1; fi
    "${CC:-cc}" -std=c23 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DX4_OPENORBIS -DX4_PUBLIC=0 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 \
        -DX4_VIDEO_CAPACITY_1080=1 -DX4_INGRESS_HOST_TEST -DX4_RUMBLE=0 \
        -DX4_NACK_GRACE_MS=45 -DX4_STREAM_WAIT_HOST=1 -D_POSIX_C_SOURCE=200809L \
        "-DX4_FEC_BASELINE=$baseline" "-DX4_AUDIO_IMPLEMENTATION=\"$tree/src/audio/live_audio.c\"" \
        "-DX4_MEDIA_IMPLEMENTATION=\"$tree/src/media/live_media.c\"" \
        -I"$tmp/include" -Itests/host_stubs -Itests/audio_stubs ${X4_TEST_CFLAGS:-} \
        tests/live_audio_fec_test.c "$tree/src/media/rtp_reorder.c" "$libopus" -lm -o "$tmp/$variant/test"
    "$tmp/$variant/test" "$tmp/$variant" 'libopus 1.5.2' | tee "$tmp/$variant/log"
done
cmp -- "$tmp/base/no_loss.pcm" "$tmp/new/no_loss.pcm"
cmp -- "$tmp/base/no_fec_5pct.pcm" "$tmp/new/no_fec_5pct.pcm"
echo 'PASS no loss and no-LBRR control PCM are byte-identical to T40b'
python3 - "$tmp/base/log" "$tmp/new/log" <<'PY'
from pathlib import Path
import sys


def parse(path):
    rows = Path(path).read_text().splitlines()
    latency, = (row for row in rows if row.startswith('LATENCY '))
    encoders = [row for row in rows if row.startswith(('ENCODER ', 'ALIGNMENT '))]
    cases = {}
    for row in rows:
        if row.startswith('CASE '):
            _, name, *fields = row.split()
            assert name not in cases
            cases[name] = dict(field.split('=', 1) for field in fields)
    return latency, encoders, cases


latency_base, encoders_base, base = parse(sys.argv[1])
latency_new, encoders_new, new = parse(sys.argv[2])
assert latency_base == latency_new and encoders_base == encoders_new
assert latency_new == 'LATENCY first_audio_us=40000 target_samples=2400 block_samples=512'
expected = {'no_loss': (0, 0), 'single_1pct': (5, 5), 'single_3pct': (15, 15),
            'single_5pct': (25, 25), 'pairs': (24, 12), 'burst3': (24, 8),
            'late': (1, 1), 'no_fec_5pct': (25, 0)}
assert base.keys() == new.keys() == expected.keys()
print('T37_SNR_COLUMNS case,losses,recovered,attempts,lbrr,plc_base_samples,plc_new_samples,'
      'whole_clip_base_db,whole_clip_new_db,lost_windows_base_db,lost_windows_new_db,'
      'clean_whole_base_db,clean_whole_new_db,clean_lost_base_db,clean_lost_new_db')
for name, (missing, groups) in expected.items():
    before, after = base[name], new[name]
    recovered = int(after['recovered'])
    assert int(before['recovered']) == 0 and 0 <= recovered <= groups
    attempts = int(after['attempts'])
    lbrr = int(after['lbrr'])
    assert recovered <= attempts <= groups and attempts <= lbrr <= 500 - missing
    assert int(after['recovered_samples']) == recovered * 960
    assert int(before['missing']) == int(after['missing']) == missing
    assert int(before['plc_samples']) == missing * 960
    assert int(after['plc_samples']) == (missing - recovered) * 960
    assert int(before['late']) == int(after['late']) == (name == 'late')
    assert int(before['samples']) == int(after['samples']) == 480000
    if name.startswith('single_'):
        assert recovered and float(after['snr_lost']) > float(before['snr_lost']), (name, before, after)
    if name in ('pairs', 'burst3'):
        assert recovered and float(after['snr_lost']) >= float(before['snr_lost']), (name, before, after)
    print('T37_SNR', name, missing, recovered, attempts, lbrr, before['plc_samples'], after['plc_samples'],
          before['snr'], after['snr'], before['snr_lost'], after['snr_lost'], before['clean_snr'],
          after['clean_snr'], before['clean_snr_lost'], after['clean_snr_lost'], sep=',')
print('PASS real Opus FEC recovery, burst quality, late RTP discard and first-audio timing')
PY
