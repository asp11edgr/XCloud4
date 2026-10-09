#!/usr/bin/env bash
set -Eeuo pipefail
umask 022
SOURCE='http://10.0.2.2:8765'
BASE="$HOME/.local/share/xcloud4"
CONF="$HOME/.config/xcloud4"
PROJECT="$HOME/Projects/XCloud4"
SDK_SHA='3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526'
mkdir -p "$BASE" "$CONF" "$HOME/Projects"
LOG="$BASE/preparacion-usuario-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
send_status() {
python3 - "$SOURCE" "$1" "$LOG" <<'PY'
import json, pathlib, sys, urllib.request
source, phase, filename = sys.argv[1:]
payload = json.dumps({'phase': phase, 'mode':'user', 'log': pathlib.Path(filename).read_text(errors='replace')[-160000:]}).encode()
try:
    urllib.request.urlopen(urllib.request.Request(source+'/status',data=payload,headers={'Content-Type':'application/json'},method='POST'),timeout=10).close()
except Exception as exc:
    print('Aviso al enviar el registro local:',exc)
PY
}
trap 'send_status "error en linea $LINENO"' ERR
[[ $(id -u) != 0 ]] || { echo 'Usa tu sesión normal de Lubuntu.'; exit 1; }
[[ $(uname -m) == x86_64 ]] || exit 1
[[ $(df -Pk "$HOME" | awk 'NR==2 {print $4}') -ge 3145728 ]] || { echo 'Se necesitan al menos 3 GiB libres en Lubuntu.'; exit 1; }
if [[ -e "$PROJECT" && ! -f "$PROJECT/.xcloud4-prepared" ]]; then
    echo "Ya existe $PROJECT; se conserva y la preparación se detiene."; exit 1
fi
echo 'XCloud4: preparación sin permisos de administrador'
APTBASE="$BASE/apt"
TOOLS="$BASE/tools"
mkdir -p "$APTBASE/lists/partial" "$APTBASE/cache/archives/partial" "$TOOLS" "$BASE/bin"
cat > "$APTBASE/user.conf" <<EOF
Dir::State::lists "$APTBASE/lists";
Dir::Cache "$APTBASE/cache";
Dir::Cache::archives "$APTBASE/cache/archives";
Dir::Log "$APTBASE";
Debug::NoLocking "true";
APT::Sandbox::User "$(id -un)";
#clear APT::Update::Post-Invoke;
#clear APT::Update::Post-Invoke-Success;
#clear DPkg::Post-Invoke;
EOF
export APT_CONFIG="$APTBASE/user.conf"
echo 'Descargando índices en la carpeta de tu usuario...'
apt-get -c "$APTBASE/user.conf" update
LLVM=(clang lld)
if apt-cache -c "$APTBASE/user.conf" policy clang-18 | grep -Eq 'Candidate: [^ ]' && ! apt-cache -c "$APTBASE/user.conf" policy clang-18 | grep -q 'Candidate: (none)' && \
   apt-cache -c "$APTBASE/user.conf" policy lld-18 | grep -Eq 'Candidate: [^ ]' && ! apt-cache -c "$APTBASE/user.conf" policy lld-18 | grep -q 'Candidate: (none)'; then
    LLVM=(clang-18 lld-18)
fi
echo 'Descargando los paquetes y sus dependencias; no se instalarán en el sistema...'
apt-get -c "$APTBASE/user.conf" --download-only --no-install-recommends --yes install "${LLVM[@]}" make cmake ninja-build pkg-config
echo 'Extrayendo herramientas dentro de tu usuario...'
for package in "$APTBASE/cache/archives/"*.deb; do
    [[ -e "$package" ]] || continue
    dpkg-deb --extract "$package" "$TOOLS"
done
unset APT_CONFIG
export LD_LIBRARY_PATH="$TOOLS/usr/lib/x86_64-linux-gnu:$TOOLS/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
LLVM_DIR=$(find "$TOOLS/usr/lib" -maxdepth 1 -type d -name 'llvm-*' | sort -V | tail -n 1)
[[ -n "$LLVM_DIR" ]] || { echo 'No se encontró LLVM en los paquetes extraídos.'; exit 1; }
for name in clang clang++ ld.lld; do
    ln -sfn "$LLVM_DIR/bin/$name" "$BASE/bin/$name"
done
python3 - "$SOURCE" "$BASE" <<'PY'
import pathlib,sys,urllib.request
source,directory=sys.argv[1:]
for remote,local in [('sdk.tar.gz','toolchain-llvm-18.tar.gz'),('project.tar.gz','XCloud4.tar.gz')]:
    urllib.request.urlretrieve(source+'/'+remote,pathlib.Path(directory)/local)
PY
echo "$SDK_SHA  $BASE/toolchain-llvm-18.tar.gz" | sha256sum --check --strict
PROJECT_SHA=$(python3 - "$SOURCE" <<'PY'
import json,sys,urllib.request
print(json.load(urllib.request.urlopen(sys.argv[1]+'/manifest.json',timeout=10))['project_sha256'])
PY
)
echo "$PROJECT_SHA  $BASE/XCloud4.tar.gz" | sha256sum --check --strict
SDK="$BASE/OpenOrbis/PS4Toolchain"
if [[ ! -e "$SDK" ]]; then
    tar -xzf "$BASE/toolchain-llvm-18.tar.gz" -C "$BASE" \
        OpenOrbis/PS4Toolchain/include OpenOrbis/PS4Toolchain/lib OpenOrbis/PS4Toolchain/bin/linux \
        OpenOrbis/PS4Toolchain/link.x OpenOrbis/PS4Toolchain/LICENSE OpenOrbis/PS4Toolchain/README.md
    printf '%s\n' "$SDK_SHA" > "$BASE/sdk.sha256"
elif [[ ! -f "$BASE/sdk.sha256" || $(cat "$BASE/sdk.sha256") != "$SDK_SHA" ]]; then
    echo 'Hay un SDK preexistente distinto; se conserva.'; exit 1
fi
chmod u+x "$SDK/bin/linux/create-fself" "$SDK/bin/linux/create-gp4" "$SDK/bin/linux/PkgTool.Core" "$SDK/bin/linux/readoelf"
if [[ ! -d "$PROJECT" ]]; then
    tar -xzf "$BASE/XCloud4.tar.gz" -C "$HOME/Projects"
    touch "$PROJECT/.xcloud4-prepared"
fi
printf 'export OO_PS4_TOOLCHAIN=%q\nexport PATH=%q:"$PATH"\nexport LD_LIBRARY_PATH=%q:"${LD_LIBRARY_PATH:-}"\n' \
    "$SDK" "$BASE/bin:$TOOLS/usr/bin:$SDK/bin/linux" "$TOOLS/usr/lib/x86_64-linux-gnu:$TOOLS/lib/x86_64-linux-gnu:$LLVM_DIR/lib" > "$CONF/env.sh"
PROFILE_LINE='[ ! -f "$HOME/.config/xcloud4/env.sh" ] || . "$HOME/.config/xcloud4/env.sh"'
grep -Fqx "$PROFILE_LINE" "$HOME/.bashrc" || printf '\n# XCloud4\n%s\n' "$PROFILE_LINE" >> "$HOME/.bashrc"
source "$CONF/env.sh"
chmod u+x "$PROJECT/scripts/"*.sh
if [[ ! -d "$PROJECT/.git" ]]; then git -C "$PROJECT" init -b main; fi
git -C "$PROJECT" config core.autocrlf false
echo 'Herramientas disponibles:'
git --version
clang --version | head -n 1
ld.lld --version
make --version | head -n 1
cmake --version | head -n 1
ninja --version
python3 --version
df -h /
printf '\nSDK: %s\nProyecto: %s\n' "$SDK" "$PROJECT"
printf '\nEntorno preparado dentro de tu usuario. No se ha compilado ni probado en la PS4.\n'
send_status 'ready'
