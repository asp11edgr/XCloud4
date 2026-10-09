#!/usr/bin/env bash
set -euo pipefail
[[ ! -f "$HOME/.config/xcloud4/env.sh" ]] || source "$HOME/.config/xcloud4/env.sh"
printf '\nSistema\n'
lsb_release -ds
uname -m
printf '\nEspacio y memoria\n'
df -h /
free -h
printf '\nHerramientas\n'
for tool in git clang clang++ ld.lld make cmake ninja python3; do
    command -v "$tool" || true
done
printf '\nSDK\n%s\n' "${OO_PS4_TOOLCHAIN:-NO CONFIGURADO}"
