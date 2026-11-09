#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf -- "$TEMP"' EXIT
CC=${CC:-cc}
flags=(-std=c2x -O2 -g -Wall -Wextra -Werror -DX4_RECEIVE_DIAG_HOST_TEST -pthread -D_DEFAULT_SOURCE -DX4_PUBLIC="${X4_PUBLIC:-0}")
"$CC" "${flags[@]}" "$ROOT/tests/rtc_receive_diag_test.c" "$ROOT/src/streaming/rtc_receive_diag.c" -o "$TEMP/receive-test"
"$TEMP/receive-test" "$TEMP/ice.json" | tee "$TEMP/receive.log"
if [[ ${X4_PUBLIC:-0} == 0 ]]; then
    grep -F 'ventana_ms=10000 udp_max_us=184000 video_rtp_max_us=190000 rangos=32 omitidos=2 pli=1' "$TEMP/receive.log"
    grep -F 'rango=1 detectado_ms=2 nack_ms=2 reintento_ms=22 retorno_ms=190' "$TEMP/receive.log"
    grep -F 'pli=1 relativo_ms=193' "$TEMP/receive.log"
    test "$(grep -c '^XCloud4: arranque43c ventana_ms=10000' "$TEMP/receive.log")" -eq 1
    test "$(grep -c '^XCloud4: arranque43c rango=' "$TEMP/receive.log")" -eq 32
else
    ! grep -q '^XCloud4: arranque43c' "$TEMP/receive.log"
fi
python3 - "$TEMP/ice.json" "$TEMP/receive.log" "${X4_PUBLIC:-0}" <<'PY'
import json,pathlib,re,sys
text=pathlib.Path(sys.argv[1]).read_text();data=json.loads(text)
log=pathlib.Path(sys.argv[2]).read_text()
selected=[line for line in log.splitlines() if line.startswith('XCloud4: ICE par seleccionado: ')]
remote=[line for line in log.splitlines() if line.startswith('XCloud4: ICE candidato remoto: ')]
pair_labels=['local=srflx/udp remote=host/udp',
             'local=host/udp remote=unknown/unknown',
             'local=host/udp remote=relay/tcp']
remote_labels=['remote=host/udp','remote=srflx/udp','remote=relay/tcp','remote=unknown/unknown']
if int(sys.argv[3]):
    assert set(data['ice'])=={'remote_candidate_counts','selected_pair_classification'}
    assert data['ice']['remote_candidate_counts']==dict(host=1,srflx=1,relay=1,prflx=0,unknown=1)
    assert data['ice']['selected_pair_classification']==dict(remote_type='relay',transport='TCP')
    assert not re.search(r'\b(?:\d{1,3}\.){3}\d{1,3}\b',text)
    assert not re.search(r'(?<![0-9A-Za-z])(?:[0-9A-Fa-f]{0,4}:){2,}[0-9A-Fa-f:]*',text)
else:
    assert data['ice']['remote_candidates']==[
        dict(ip_prefix=prefix,type=kind,transport=proto)
        for prefix,kind,proto in [('192.0.x.x','host','udp'),('203.0.x.x','srflx','udp'),
                                  ('2001:db8::/32','relay','tcp'),('198.51.x.x','unknown','unknown')]]
    assert data['ice']['selected_candidate_pair']==dict(
        local='host/udp',remote='relay/tcp',local_prefix='2001:db8::/32',remote_prefix='2001:db8::/32')
    pair_prefixes=[('203.0.x.x','192.0.x.x'),('-','-'),('2001:db8::/32','2001:db8::/32')]
    pair_labels=[f'{pair} local_prefix={left} remote_prefix={right}'
                 for pair,(left,right) in zip(pair_labels,pair_prefixes)]
    remote_labels=[f'{line} remote_prefix={prefix}'
                   for line,prefix in zip(remote_labels,['192.0.x.x','203.0.x.x','2001:db8::/32','198.51.x.x'])]
assert selected==['XCloud4: ICE par seleccionado: '+line for line in pair_labels]
assert remote==['XCloud4: ICE candidato remoto: '+line for line in remote_labels]
for raw in ('candidate:', '192.0.2.50', '203.0.113.5', '198.51.100.',
            '2001:db8::5', '2001:db8:ab12', '2001:db8::7', '2001:db8::9',
            '50000','50001','50002','50003','54321','example.invalid','game_server_port'):
    assert raw not in text and raw not in '\n'.join(selected+remote), raw
assert 'XCloud4: Servidor de juego (envia video)' not in log
print('PASS real ICE IPv4/IPv6 log and JSON privacy for edition',sys.argv[3])
PY
python3 "$ROOT/tests/check_ice_log_formats46b.py" --diag-only --public "${X4_PUBLIC:-0}" "$TEMP/receive-test"
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
        if "$TEMP/receive-sanitized" >"$TEMP/sanitizer-run.log" 2>&1; then
            cat "$TEMP/sanitizer-run.log"
        else
            status=$?
            cat "$TEMP/sanitizer-run.log"
            if [[ "$sanitizer" == thread ]] && grep -Eq 'FATAL: ThreadSanitizer: (unexpected memory mapping|unsupported)' "$TEMP/sanitizer-run.log"; then
                printf 'UNAVAILABLE sanitizer %s runtime; not a race-free result\n' "$sanitizer"
            else
                exit "$status"
            fi
        fi
    done
fi
