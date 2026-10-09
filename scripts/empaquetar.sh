#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -Eeuo pipefail
cd "$(dirname "$0")/.."
: "${OO_PS4_TOOLCHAIN:?Carga ~/.config/xcloud4/env.sh primero}"
TITLE='XCloud4'
TITLE_ID='XCLD00001'
CONTENT_ID='IV0000-XCLD00001_00-XCLOUD4APP000000'
VERSION='00.10'
TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
# LibOrbisPkg is distributed as a self-contained .NET executable.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
HOSTLIBS="${X4_HOSTLIBS:-$HOME/.local/share/xcloud4/hostlibs}"
if [[ -d "$HOSTLIBS/usr/lib/x86_64-linux-gnu" ]]; then
    export LD_LIBRARY_PATH="$HOSTLIBS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
mkdir -p build/package/sce_sys dist
cp build/eboot.bin build/package/eboot.bin
cp assets/icon0.png build/package/sce_sys/icon0.png
cp LICENSE build/package/LICENSE.txt
cp THIRD_PARTY_NOTICES.md build/package/THIRD_PARTY_NOTICES.md
SFO='build/package/sce_sys/param.sfo'
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
entry() { "$TOOLS/PkgTool.Core" sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4"; }
entry APP_TYPE Integer 4 1
entry APP_VER Utf8 8 "$VERSION"
entry ATTRIBUTE Integer 4 0
entry CATEGORY Utf8 4 gd
entry CONTENT_ID Utf8 48 "$CONTENT_ID"
entry DOWNLOAD_DATA_SIZE Integer 4 0
entry SYSTEM_VER Integer 4 0
entry TITLE Utf8 128 "$TITLE"
entry TITLE_ID Utf8 12 "$TITLE_ID"
entry VERSION Utf8 8 "$VERSION"
DIST="$PWD/dist"
(
    cd build/package
    "$TOOLS/create-gp4" -out xcloud4.gp4 --content-id="$CONTENT_ID" \
        --files 'eboot.bin sce_sys/param.sfo sce_sys/icon0.png LICENSE.txt THIRD_PARTY_NOTICES.md'
    "$TOOLS/PkgTool.Core" pkg_build xcloud4.gp4 "$DIST"
)
cp "dist/$CONTENT_ID.pkg" dist/XCloud4-0.1.0.pkg
sha256sum dist/XCloud4-0.1.0.pkg > dist/XCloud4-0.1.0.pkg.sha256
printf '\nPaquete: %s/dist/XCloud4-0.1.0.pkg\n' "$PWD"
