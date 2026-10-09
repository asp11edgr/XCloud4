#!/usr/bin/env bash
set -Eeuo pipefail
SOURCE='http://10.0.2.2:8765'
BASE="$HOME/.local/share/xcloud4"
CONF="$HOME/.config/xcloud4"
PROJECT="$HOME/Projects/XCloud4"
SDK_SHA='3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526'
[[ $(id -u) != 0 ]] || { echo 'Este paso debe ejecutarse como el usuario del proyecto.'; exit 1; }
mkdir -p "$BASE/bin" "$CONF" "$HOME/Projects"
LOG="$BASE/sdk-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
if [[ -e "$PROJECT" && ! -f "$PROJECT/.xcloud4-prepared" ]]; then
    echo 'Se conserva el proyecto preexistente; la preparación se detiene.'; exit 1
fi
python3 - "$SOURCE" "$BASE" <<'PY'
import pathlib,sys,urllib.request
source,directory=sys.argv[1:]
for remote,local in [('sdk.tar.gz','toolchain-llvm-18.tar.gz'),('project.tar.gz','XCloud4.tar.gz'),('control.pub','control.pub')]:
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
    echo 'Hay un SDK distinto preexistente; se conserva.'; exit 1
fi
chmod u+x "$SDK/bin/linux/create-fself" "$SDK/bin/linux/create-gp4" "$SDK/bin/linux/PkgTool.Core" "$SDK/bin/linux/readoelf"
if [[ ! -d "$PROJECT" ]]; then
    tar -xzf "$BASE/XCloud4.tar.gz" -C "$HOME/Projects"
    touch "$PROJECT/.xcloud4-prepared"
fi
if command -v clang-18 >/dev/null; then
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
install -d -m 700 "$HOME/.ssh"
KEY=$(cat "$BASE/control.pub")
grep -Fq "$KEY" "$HOME/.ssh/authorized_keys" 2>/dev/null || printf 'restrict %s\n' "$KEY" >> "$HOME/.ssh/authorized_keys"
chmod 600 "$HOME/.ssh/authorized_keys"
git --version
clang --version | head -n 1
ld.lld --version
make --version | head -n 1
cmake --version | head -n 1
ninja --version
python3 --version
df -h /
printf '\nSDK: %s\nProyecto: %s\nEntorno preparado.\n' "$SDK" "$PROJECT"
python3 - "$LOG" <<'PY'
import json,pathlib,sys,urllib.request
payload=json.dumps({'phase':'ready','mode':'system','log':pathlib.Path(sys.argv[1]).read_text(errors='replace')[-160000:]}).encode()
urllib.request.urlopen(urllib.request.Request('http://10.0.2.2:8765/status',data=payload,headers={'Content-Type':'application/json'},method='POST'),timeout=10).close()
PY
