#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -Eeuo pipefail
cd "$(dirname "$0")/.."
: "${OO_PS4_TOOLCHAIN:?Carga ~/.config/xcloud4/env.sh primero}"
# Explicit probe or public identity, defaulting to the unchanged normal metadata.
PUBLIC_FLAG=${X4_PUBLIC:-0}
if [[ "$PUBLIC_FLAG" == 1 && -z "${X4_PACKAGE_MODE:-}" ]]; then
    PACKAGE_MODE=public
else
    PACKAGE_MODE=${X4_PACKAGE_MODE:-normal}
fi
PROBE_VARIANT=${X4_PROBE_VARIANT:-}
PACKAGE_SUFFIX=${X4_PACKAGE_SUFFIX:-}
STREAM_PROFILE=${X4_STREAM_PROFILE:-0}
T26_FLAG=${X4_PROBE_T26:-0}
T27_FLAG=${X4_PROBE_T27:-0}
case "$T27_FLAG" in 0|1) ;; *) echo 'Invalid explicit T27 flag.' >&2; exit 1 ;; esac
if [[ "$T27_FLAG" == 1 && ( "$PACKAGE_MODE" != probe || "$PROBE_VARIANT" != t27 || "$T26_FLAG" != 1 ) ]]; then
    echo 'T27 is exclusive to probe/t27 with reused T26 bank.' >&2; exit 1
fi
case "$T26_FLAG" in 0|1) ;; *) echo 'Invalid explicit T26 flag.' >&2; exit 1 ;; esac
if [[ "$T26_FLAG" == 1 && ( "$PACKAGE_MODE" != probe || "$PROBE_VARIANT" != t26 && "$PROBE_VARIANT" != t27 ) ]]; then
    echo 'T26 is exclusive to probe mode with variant t26.' >&2; exit 1
fi
case "$STREAM_PROFILE" in 0|1|2) ;; *) echo 'Invalid explicit streaming profile.' >&2; exit 1 ;; esac
# A direct probe packaging invocation must pass the same current-ELF gates as
# make all, even if a matching stale SELF was left by an earlier build. Run this
# before creating staging directories or copying anything into the package.
if [[ "$PACKAGE_MODE" == probe ]]; then
    case "$PROBE_VARIANT" in
        t27)
            STREAM_PROFILE=${X4_STREAM_PROFILE:-1}
            [[ "$T27_FLAG" == 1 && "$T26_FLAG" == 1 && "$STREAM_PROFILE" == 1 ]] || { echo 'T27 requires both bank flags and streaming profile1.' >&2; exit 1; }
            make build/eboot.bin X4_VIDEO_PROBE=1 X4_PROBE_T26=1 X4_PROBE_T27=1 X4_VIDEO_CAPACITY_1080=1 \
                X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=0 X4_INGRESS_MODE=mpsc X4_STREAM_PROFILE=1 X4_PUBLIC=0
            ;;
        t26)
            STREAM_PROFILE=${X4_STREAM_PROFILE:-1}
            [[ "$T26_FLAG" == 1 && "$STREAM_PROFILE" == 1 ]] || { echo 'T26 requires explicit X4_PROBE_T26=1 and streaming profile 1.' >&2; exit 1; }
            make build/eboot.bin X4_VIDEO_PROBE=1 X4_PROBE_T26=1 X4_VIDEO_CAPACITY_1080=1 \
                X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=0 X4_INGRESS_MODE=mpsc X4_STREAM_PROFILE=1 X4_PUBLIC=0
            ;;
        off|on)
            [[ "$T26_FLAG" == 0 && "$STREAM_PROFILE" == 0 ]] || { echo 'Legacy local probe requires streaming profile 0 and no T26 flag.' >&2; exit 1; }
            if [[ "$PROBE_VARIANT" == on ]]; then PROBE_CAPACITY=1; else PROBE_CAPACITY=0; fi
            make build/eboot.bin X4_VIDEO_PROBE=1 X4_VIDEO_CAPACITY_1080="$PROBE_CAPACITY" \
                X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=0 X4_INGRESS_MODE=mpsc X4_STREAM_PROFILE=0 X4_PUBLIC=0
            ;;
        *) echo 'Probe packaging requires explicit off/on/t26/t27 variant.' >&2; exit 1 ;;
    esac
elif [[ "$PACKAGE_MODE" == public ]]; then
    STREAM_PROFILE=${X4_STREAM_PROFILE:-1}
    [[ "$STREAM_PROFILE" == 1 ]] || { echo 'Public package requires streaming profile 1.' >&2; exit 1; }
    [[ -z "$PACKAGE_SUFFIX" || "$PACKAGE_SUFFIX" == -publico ]] || { echo 'Public package allows only empty or -publico suffix.' >&2; exit 1; }
    make build/eboot.bin X4_VIDEO_PROBE=0 X4_VIDEO_CAPACITY_1080=1 \
        X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=1 X4_INGRESS_MODE=mpsc \
        X4_STREAM_PROFILE=1 X4_PUBLIC=1
elif [[ "$PACKAGE_MODE" == normal && "$STREAM_PROFILE" != 0 ]]; then
    if [[ "$STREAM_PROFILE" == 1 ]]; then EXPECTED_SUFFIX=-720p60; else EXPECTED_SUFFIX=-1080p60; fi
    [[ -z "$PACKAGE_SUFFIX" && "$STREAM_PROFILE" == 1 || "$PACKAGE_SUFFIX" == "$EXPECTED_SUFFIX" || ( "$STREAM_PROFILE" == 1 && "$PACKAGE_SUFFIX" == -perfiles ) ]] || { echo 'Package suffix does not match streaming profile.' >&2; exit 1; }
    # Re-run current-ELF audits before packaging even if a stale SELF exists.
    make build/eboot.bin X4_VIDEO_PROBE=0 X4_VIDEO_CAPACITY_1080=1 \
        X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=1 X4_INGRESS_MODE=mpsc \
        X4_STREAM_PROFILE="$STREAM_PROFILE" X4_PUBLIC=0
fi
IDENTITY=$(python3 scripts/package_identity.py verify --mode "$PACKAGE_MODE" \
    --variant "$PROBE_VARIANT" --suffix="$PACKAGE_SUFFIX" --format lines)
mapfile -t FIELDS <<< "$IDENTITY"
[[ ${#FIELDS[@]} == 9 ]] || { echo 'Incomplete verified package identity' >&2; exit 1; }
TITLE=${FIELDS[0]}; TITLE_ID=${FIELDS[1]}; CONTENT_ID=${FIELDS[2]}
VERSION=${FIELDS[3]}; PACKAGE_VERSION=${FIELDS[4]}; PACKAGE_NAME=${FIELDS[5]}
PACKAGE_ROOT="$PWD/${FIELDS[6]}"; DIST="$PWD/${FIELDS[7]}"; INCLUDE_DEMO=${FIELDS[8]}
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
mkdir -p "$PACKAGE_ROOT/sce_sys" "$PACKAGE_ROOT/sce_module" "$PACKAGE_ROOT/assets" "$DIST"
if [[ "$INCLUDE_DEMO" == 1 ]]; then
    [[ -s assets/sample.h264 ]] || { echo 'Falta assets/sample.h264; ejecuta scripts/generar-muestra.sh.' >&2; exit 1; }
    cp assets/sample.h264 "$PACKAGE_ROOT/assets/sample.h264"
fi
mkdir -p "$PACKAGE_ROOT/assets/fonts"
FONT_FILES=(noto_sans.x4font inter.x4font OFL.txt Inter-OFL.txt README.md SOURCE.json ATLAS.json)
for asset in "${FONT_FILES[@]}"; do
    [[ -s "assets/fonts/$asset" ]] || { echo "Missing font asset: $asset" >&2; exit 1; }
    cp "assets/fonts/$asset" "$PACKAGE_ROOT/assets/fonts/$asset"
done
cp build/eboot.bin "$PACKAGE_ROOT/eboot.bin"
# Revalidate the staged SELF, rather than trusting the copy/source preflight.
python3 scripts/package_identity.py verify --mode "$PACKAGE_MODE" \
    --variant "$PROBE_VARIANT" --suffix="$PACKAGE_SUFFIX" \
    --self "$PACKAGE_ROOT/eboot.bin" >/dev/null
for module in "${RUNTIME_FILES[@]}"; do
    cp "$X4_RUNTIME_MODULES/$module" "$PACKAGE_ROOT/sce_module/$module"
done
if [[ "$PACKAGE_MODE" == normal ]]; then
    cp assets/icon0-lab.png "$PACKAGE_ROOT/sce_sys/icon0.png"
else
    cp assets/icon0.png "$PACKAGE_ROOT/sce_sys/icon0.png"
fi
cp LICENSE "$PACKAGE_ROOT/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$PACKAGE_ROOT/THIRD_PARTY_NOTICES.md"
mkdir -p "$PACKAGE_ROOT/licenses"
LICENSE_FILES=(docs/licenses/*)
cp "${LICENSE_FILES[@]}" "$PACKAGE_ROOT/licenses/"
PACKAGE_FILES='eboot.bin sce_module/libSceFios2.prx sce_module/libc.prx sce_sys/param.sfo sce_sys/icon0.png LICENSE.txt THIRD_PARTY_NOTICES.md'
if [[ "$INCLUDE_DEMO" == 1 ]]; then
    PACKAGE_FILES='eboot.bin assets/sample.h264 sce_module/libSceFios2.prx sce_module/libc.prx sce_sys/param.sfo sce_sys/icon0.png LICENSE.txt THIRD_PARTY_NOTICES.md'
fi
if [[ "$PACKAGE_MODE" != probe ]]; then
    if [[ "$PACKAGE_MODE" == normal ]]; then
        cp assets/pic1-lab.png "$PACKAGE_ROOT/sce_sys/pic1.png"
    else
        cp assets/pic1.png "$PACKAGE_ROOT/sce_sys/pic1.png"
    fi
    PACKAGE_FILES+=" sce_sys/pic1.png"
fi
for asset in "${FONT_FILES[@]}"; do PACKAGE_FILES+=" assets/fonts/$asset"; done
if [[ "$PACKAGE_MODE" == normal ]]; then
    mkdir -p "$PACKAGE_ROOT/assets/scripts"
    cp assets/scripts/*.lua "$PACKAGE_ROOT/assets/scripts/"
    for script in assets/scripts/*.lua; do
        PACKAGE_FILES+=" assets/scripts/${script##*/}"
    done
fi
for license in "${LICENSE_FILES[@]}"; do
    PACKAGE_FILES+=" licenses/${license##*/}"
done
SFO="$PACKAGE_ROOT/sce_sys/param.sfo"
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
if [[ "$PACKAGE_MODE" != probe ]]; then
    entry X4_PRODUCT_VERSION Utf8 48 "$PACKAGE_VERSION"
fi
# Fail before GP4/PKG creation if the tool-generated SFO has wrong semantics.
python3 scripts/package_identity.py sfo --mode "$PACKAGE_MODE" \
    --variant "$PROBE_VARIANT" --suffix="$PACKAGE_SUFFIX" \
    --sfo-file "$SFO" >/dev/null
(
    cd "$PACKAGE_ROOT"
    "$TOOLS/create-gp4" -out xcloud4.gp4 --content-id="$CONTENT_ID" \
        --files "$PACKAGE_FILES"
    # Preserve the tool's directory tree and declare the nested runtime font
    # directory explicitly; create-gp4 does not enumerate these directories.
    python3 - "$PACKAGE_MODE" <<'PY'
import sys
from pathlib import Path
import xml.etree.ElementTree as ET
package_mode = sys.argv[1]
project = Path('xcloud4.gp4')
tree = ET.parse(project)
roots = [node for node in tree.iter() if node.tag.rsplit('}', 1)[-1] == 'rootdir']
if len(roots) != 1:
    raise SystemExit('Expected one GP4 root directory declaration.')
rootdir = roots[0]
namespace = rootdir.tag[:-len('rootdir')]
def directory(parent, name):
    found = [node for node in parent if node.tag == namespace + 'dir' and node.get('targ_name') == name]
    if len(found) > 1:
        raise SystemExit('Duplicate GP4 directory: ' + name)
    return found[0] if found else ET.SubElement(parent, namespace + 'dir', {'targ_name': name})
directory(rootdir, 'licenses')
directory(directory(rootdir, 'assets'), 'fonts')
if package_mode == 'normal':
    directory(directory(rootdir, 'assets'), 'scripts')
ET.indent(tree, space='\t')
tree.write(project, encoding='utf-8', xml_declaration=True)
PY
    "$TOOLS/PkgTool.Core" pkg_build xcloud4.gp4 "$DIST"
)
cp "$DIST/$CONTENT_ID.pkg" "$DIST/$PACKAGE_NAME"
sha256sum "$DIST/$PACKAGE_NAME" > "$DIST/$PACKAGE_NAME.sha256"
printf '\nPaquete: %s/%s\n' "$DIST" "$PACKAGE_NAME"
