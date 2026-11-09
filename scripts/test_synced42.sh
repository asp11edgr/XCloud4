#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
: "${X4_SYNC42_BASE:?Set X4_SYNC42_BASE to the verified T34 source tree}"
task_tmp=$(mktemp -d /tmp/x4-synced42-XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
python3 - "$X4_SYNC42_BASE" "$task_tmp" <<'PY'
import hashlib,pathlib,sys
base,tmp=map(pathlib.Path,sys.argv[1:])
expected={"video_pipeline.c":"9155d6f6748006d3a205ae974b3111ef14551051186fb474c71505915e5b0812",
          "video_pipeline.h":"db07bd161b5a3659acf8247fdb89af0df8343558ee06364c9d3f028ba337d061"}
for name,sha in expected.items():
    raw=(base/"src/video"/name).read_bytes()
    actual=hashlib.sha256(raw).hexdigest()
    if actual!=sha:raise SystemExit("T34 baseline SHA mismatch: "+name)
    text=raw.decode().replace("x4_video_pipeline_","t34_video_pipeline_")
    if name.endswith(".c"):text=text.replace('"video_pipeline.h"','"legacy_video_pipeline.h"')
    (tmp/("legacy_"+name)).write_text(text)
    print("BASE_SHA256",name,actual)
engine=pathlib.Path("tests/synced42_engine.c").read_text()
engine=engine.replace('"../src/video/video_pipeline.h"','"legacy_video_pipeline.h"')
engine=engine.replace("x4_video_pipeline_","t34_video_pipeline_").replace("synced42_engine(","legacy42_engine(")
(tmp/"legacy_engine.c").write_text(engine)
PY
common=(-std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests -Isrc/video -I"$task_tmp")
for variant in plain sanitized; do
    flags=()
    link_flags=()
    if [[ "$variant" == sanitized ]]; then
        flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        link_flags=(-no-pie)
    fi
    "${CC:-gcc}" "${common[@]}" "${flags[@]}" -DT42_LEGACY=1 -c "$task_tmp/legacy_engine.c" -o "$task_tmp/legacy-engine-$variant.o"
    "${CC:-gcc}" "${common[@]}" "${flags[@]}" tests/synced42_test.c tests/synced42_engine.c \
        src/video/video_pipeline.c "$task_tmp/legacy_video_pipeline.c" "$task_tmp/legacy-engine-$variant.o" \
        "${link_flags[@]}" -Wl,--gc-sections -o "$task_tmp/test-$variant"
    echo "VARIANT $variant"
    "$task_tmp/test-$variant"
done
echo 'PASS synced42 functional safety and exact T34 legacy comparison (plain + ASan/UBSan)'
