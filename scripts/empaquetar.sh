#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -Eeuo pipefail
cd "$(dirname "$0")/.."
: "${OO_PS4_TOOLCHAIN:?Carga ~/.config/xcloud4/env.sh primero}"
TITLE='XCloud4'
TITLE_ID='XCLD00001'
CONTENT_ID='IV0000-XCLD00001_00-XCLOUD4APP000000'
VERSION='00.60'
PACKAGE_VERSION='0.6.0'
TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
# The PS4 startup loader requires the auxiliary OpenOrbis modules before main().
# Keep compiled module binaries in an external local directory, never in Git.
: "${X4_RUNTIME_MODULES:?Define X4_RUNTIME_MODULES con los módulos locales de PS4}"
RUNTIME_FILES=(libSceFios2.prx libc.prx)
for module in "${RUNTIME_FILES[@]}"; do
    [[ -s "$X4_RUNTIME_MODULES/$module" ]] || { echo "Falta $module; no se genera un paquete incompleto." >&2; exit 1; }
done
python3 - "$X4_RUNTIME_MODULES" "${RUNTIME_FILES[@]}" <<'PY'
import pathlib,sys
for name in sys.argv[2:]:
    module=pathlib.Path(sys.argv[1])/name
    if module.read_bytes()[:4] != bytes.fromhex('4f153d1d'):
        raise SystemExit(f'{name} debe ser un módulo SELF. Un ELF obtenido por FTP requiere conversión previa.')
PY
# LibOrbisPkg is distributed as a self-contained .NET executable.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
HOSTLIBS="${X4_HOSTLIBS:-$HOME/.local/share/xcloud4/hostlibs}"
if [[ -d "$HOSTLIBS/usr/lib/x86_64-linux-gnu" ]]; then
    export LD_LIBRARY_PATH="$HOSTLIBS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
[[ -s assets/sample.h264 ]] || { echo 'Falta assets/sample.h264; ejecuta scripts/generar-muestra.sh.' >&2; exit 1; }
mkdir -p build/package/sce_sys build/package/sce_module build/package/assets dist
cp assets/sample.h264 build/package/assets/sample.h264
cp build/eboot.bin build/package/eboot.bin
for module in "${RUNTIME_FILES[@]}"; do
    cp "$X4_RUNTIME_MODULES/$module" "build/package/sce_module/$module"
done
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
        --files 'eboot.bin assets/sample.h264 sce_module/libSceFios2.prx sce_module/libc.prx sce_sys/param.sfo sce_sys/icon0.png LICENSE.txt THIRD_PARTY_NOTICES.md'
    "$TOOLS/PkgTool.Core" pkg_build xcloud4.gp4 "$DIST"
)
cp "dist/$CONTENT_ID.pkg" "dist/XCloud4-$PACKAGE_VERSION.pkg"
sha256sum "dist/XCloud4-$PACKAGE_VERSION.pkg" > "dist/XCloud4-$PACKAGE_VERSION.pkg.sha256"
printf '\nPaquete: %s/dist/XCloud4-%s.pkg\n' "$PWD" "$PACKAGE_VERSION"
