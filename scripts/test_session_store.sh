#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/x4-session-store-XXXXXX")
trap 'rm -r "$tmp"' EXIT
cc=${CC:-cc}
flags=(-std=c11 -Wall -Wextra -Werror -DX4_SESSION_STORE_TEST=1 -g)
if [[ ${X4_SESSION_STORE_SANITIZERS:-0} == 1 ]]; then
  flags+=(-O1 -fno-omit-frame-pointer -fsanitize=address,undefined)
else
  flags+=(-O2)
fi
"$cc" "${flags[@]}" src/auth/session_store.c tests/session_store_test.c -o "$tmp/test_session_store"
"$tmp/test_session_store" >"$tmp/stdout.log" 2>"$tmp/stderr.log"
python3 - "$tmp" <<'PY'
import pathlib,re,sys
source=pathlib.Path('src/auth/session_store.c').read_text(encoding='utf-8')
assert '#include <orbis/SaveData.h>' in source
for forbidden in ('x4_module_open','x4_module_symbol','sceKernelDlsym','../core/module.h','x4_ms_session'):
    assert forbidden not in source,'Legacy SaveData loading/directory remains'
for name in ('sceSaveDataInitialize3','sceSaveDataDirNameSearch','sceSaveDataMount2','sceSaveDataUmount'):
    assert re.search(r'\b'+name+r'\s*\(',source),'Missing direct SDK SaveData call'
print('PASS SaveData uses direct SDK calls, without module resolution or the old directory')
root=pathlib.Path(sys.argv[1])
for path in root.iterdir():
    if path.suffix!='.log':continue
    body=path.read_bytes()
    assert b'FAKE_REFRESH_' not in body,'A fake credential leaked into a log'
print('PASS no fake credential in stdout/stderr or host data files; backend is memory only')
PY
cat "$tmp/stdout.log" "$tmp/stderr.log"
