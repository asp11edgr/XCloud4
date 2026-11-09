#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc-clean-17/mbedtls}
if [[ -z ${X4_T26_REAL_CLIPS_DIR:-} ]]; then
    echo 'NOT_RUN real generated T26 cache: set X4_T26_REAL_CLIPS_DIR to the existing t26-clips-r1 directory.'
    exit 0
fi
clips=$(cd -- "$X4_T26_REAL_CLIPS_DIR" && pwd -P)
test -f "$dep/library/sha256.c" && test -f "$dep/library/platform_util.c"
test -f "$repo/src/video/probe_t26_samples.h"
build=$(mktemp -d /tmp/x4-t26-cache-real.XXXXXX)
echo "T26_REAL_CACHE_TEST_ARTIFACTS $build"
mkdir "$build/xcloud4-probe"
# Host-only intermediate-directory symlink. Every .h264/.x4au leaf remains
# the original regular file (O_NOFOLLOW stays effective; nlink unchanged).
# No copy, hardlink, native path change, removal or cleanup is performed.
ln -s -- "$clips" "$build/xcloud4-probe/t26"
printf 'METHOD host-only intermediate-directory symlink: %s -> %s; original leaves, no copy/hardlink/decoder\n' "$build/xcloud4-probe/t26" "$clips" | tee "$build/method.log"
python3 -B - "$clips" "$build/input-metadata-before.json" <<'PY'
import json,os,pathlib,sys
root=pathlib.Path(sys.argv[1]);rows=[]
for path in sorted(root.iterdir()):
    if path.suffix not in ('.h264','.x4au'):continue
    assert path.is_file() and not path.is_symlink(),path
    s=path.stat();rows.append(dict(name=path.name,size=s.st_size,dev=s.st_dev,ino=s.st_ino,nlink=s.st_nlink,mtime_ns=s.st_mtime_ns,ctime_ns=s.st_ctime_ns))
assert len(rows)==10,len(rows)
pathlib.Path(sys.argv[2]).write_text(json.dumps(rows,indent=2)+'\n')
PY
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]]; then read -r -a extra <<< "$X4_TEST_CFLAGS"; fi
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1
    -DX4_STREAM_PROFILE=1 -DX4_PROBE_FILES_TESTING -I"$repo/src/video" -I"$dep/include")
"$compiler" "${flags[@]}" "${extra[@]}" "$repo/tests/probe_t26_cache_real_test.c" \
    "$repo/src/video/probe_t26_cache.c" "$repo/src/video/probe_source.c" \
    "$repo/src/video/probe_files.c" "$repo/src/video/probe_hash.c" "$repo/src/video/stream_sps.c" \
    "$dep/library/sha256.c" "$dep/library/platform_util.c" -o "$build/cache-real-test"
set +e
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$build/cache-real-test" "$build" | tee "$build/cache-real.log"
pipeline_status=("${PIPESTATUS[@]}")
set -e
python3 -B - "$clips" "$build/input-metadata-before.json" "$build/input-metadata-after.json" <<'PY'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1]);before=json.loads(pathlib.Path(sys.argv[2]).read_text());after=[]
for old in before:
    path=root/old['name'];assert not path.is_symlink();s=path.stat()
    after.append(dict(name=path.name,size=s.st_size,dev=s.st_dev,ino=s.st_ino,nlink=s.st_nlink,mtime_ns=s.st_mtime_ns,ctime_ns=s.st_ctime_ns))
pathlib.Path(sys.argv[3]).write_text(json.dumps(after,indent=2)+'\n')
assert before==after,'Input files metadata changed during the read-only cache test'
print('PASS original ten leaves: size/inode/link-count/mtime/ctime unchanged; C validation result is reported separately; native memory/decoder untouched')
PY
if ((pipeline_status[0])); then exit "${pipeline_status[0]}"; fi
if ((pipeline_status[1])); then exit "${pipeline_status[1]}"; fi
echo 'PASS real generated cache test: five clips fully SHA/index/SPS verified; original ten inputs unchanged; no native decoder call.'
