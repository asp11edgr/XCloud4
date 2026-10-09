#!/usr/bin/env bash
set -Eeuo pipefail
umask 022
LOCAL_SOURCE='http://10.0.2.2:8765'
SDK_SHA='3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526'
BASE="$HOME/.local/share/xcloud4"
CONF="$HOME/.config/xcloud4"
PROJECT="$HOME/Projects/XCloud4"
mkdir -p "$BASE" "$CONF" "$HOME/Projects"
LOG="$BASE/preparacion-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1

send_status() {
    python3 - "$LOCAL_SOURCE" "$1" "$LOG" <<'PY'
import json, pathlib, sys, urllib.request
source, phase, logfile = sys.argv[1:]
payload = json.dumps({'phase': phase, 'log': pathlib.Path(logfile).read_text(errors='replace')[-160000:]}).encode()
try:
    urllib.request.urlopen(urllib.request.Request(source + '/status', data=payload, headers={'Content-Type': 'application/json'}, method='POST'), timeout=10).close()
except Exception as exc:
    print('No se pudo enviar el estado local:', exc)
PY
}
trap 'send_status "error en linea $LINENO"; printf "\nLa preparación se interrumpió. Registro: %s\n" "$LOG"' ERR

echo 'XCloud4: preparación de Lubuntu'
[[ $(id -u) != 0 ]] || { echo 'Ejecuta este instalador como tu usuario normal, sin sudo delante de bash.'; exit 1; }
[[ $(uname -m) == x86_64 ]] || { echo 'Este SDK requiere Linux de 64 bits.'; exit 1; }
[[ $(df -Pk "$HOME" | awk 'NR==2 {print $4}') -ge 3145728 ]] || {
    echo 'Quedan menos de 3 GiB libres en Lubuntu. Hace falta espacio antes de instalar.'; exit 1;
}
if [[ -e "$PROJECT" && ! -f "$PROJECT/.xcloud4-prepared" ]]; then
    echo "Ya existe $PROJECT. No se reemplazará un proyecto ajeno a esta preparación."; exit 1
fi

echo 'Lubuntu puede pedir ahora tu contraseña de administrador. No se envía a Windows.'
sudo -v
sudo apt-get update
LLVM_PKGS=(clang lld)
if apt-cache policy clang-18 | grep -Eq 'Candidate: [^ ]' && ! apt-cache policy clang-18 | grep -q 'Candidate: (none)' && \
   apt-cache policy lld-18 | grep -Eq 'Candidate: [^ ]' && ! apt-cache policy lld-18 | grep -q 'Candidate: (none)'; then
    LLVM_PKGS=(clang-18 lld-18)
fi
sudo apt-get install -y --no-install-recommends build-essential "${LLVM_PKGS[@]}" cmake ninja-build \
    git python3 curl ca-certificates pkg-config unzip openssh-server

python3 - "$LOCAL_SOURCE" "$BASE" <<'PY'
import pathlib, sys, urllib.request
source, directory = sys.argv[1:]
for remote, local in [('sdk.tar.gz', 'toolchain-llvm-18.tar.gz'), ('project.tar.gz', 'XCloud4.tar.gz'), ('control.pub', 'control.pub')]:
    target = pathlib.Path(directory) / local
    urllib.request.urlretrieve(source + '/' + remote, target)
PY
echo "$SDK_SHA  $BASE/toolchain-llvm-18.tar.gz" | sha256sum --check --strict
PROJECT_SHA=$(python3 - "$LOCAL_SOURCE" <<'PY'
import json, sys, urllib.request
print(json.load(urllib.request.urlopen(sys.argv[1] + '/manifest.json', timeout=10))['project_sha256'])
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
    echo 'Hay un SDK distinto en la ruta elegida. Se conserva; no se reemplazará.'; exit 1
fi
chmod u+x "$SDK/bin/linux/create-fself" "$SDK/bin/linux/create-gp4" "$SDK/bin/linux/PkgTool.Core" "$SDK/bin/linux/readoelf"
if [[ ! -d "$PROJECT" ]]; then
    tar -xzf "$BASE/XCloud4.tar.gz" -C "$HOME/Projects"
    touch "$PROJECT/.xcloud4-prepared"
fi
mkdir -p "$BASE/bin"
if [[ ${LLVM_PKGS[0]} == clang-18 ]]; then
    ln -sfn /usr/bin/clang-18 "$BASE/bin/clang"
    ln -sfn /usr/bin/clang++-18 "$BASE/bin/clang++"
    ln -sfn /usr/bin/ld.lld-18 "$BASE/bin/ld.lld"
fi
printf 'export OO_PS4_TOOLCHAIN=%q\nexport PATH=%q:"$PATH"\n' "$SDK" "$BASE/bin:$SDK/bin/linux" > "$CONF/env.sh"
PROFILE_LINE='[ ! -f "$HOME/.config/xcloud4/env.sh" ] || . "$HOME/.config/xcloud4/env.sh"'
grep -Fqx "$PROFILE_LINE" "$HOME/.bashrc" || printf '\n# XCloud4\n%s\n' "$PROFILE_LINE" >> "$HOME/.bashrc"
source "$CONF/env.sh"
chmod u+x "$PROJECT/scripts/"*.sh
if [[ ! -d "$PROJECT/.git" ]]; then git -C "$PROJECT" init -b main; fi
git -C "$PROJECT" config core.autocrlf false

# Acceso por clave desde Windows a través del puerto NAT que sólo escucha en 127.0.0.1.
install -d -m 700 "$HOME/.ssh"
KEY=$(cat "$BASE/control.pub")
grep -Fq "$KEY" "$HOME/.ssh/authorized_keys" 2>/dev/null || printf 'restrict %s\n' "$KEY" >> "$HOME/.ssh/authorized_keys"
chmod 600 "$HOME/.ssh/authorized_keys"
sudo systemctl enable --now ssh

echo 'Herramientas disponibles:'
git --version
clang --version | head -n 1
ld.lld --version
cmake --version | head -n 1
ninja --version
python3 --version
df -h /
printf '\nSDK: %s\nProyecto: %s\n' "$SDK" "$PROJECT"
printf '\nEntorno instalado. No se ha compilado ni probado aún el cliente en la PS4.\n'
send_status 'ready'
