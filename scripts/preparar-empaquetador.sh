#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -Eeuo pipefail
# Extract only; do not install this legacy library into the Ubuntu system.
BASE="$HOME/.local/share/xcloud4"
FILE='libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb'
SHA='7cf39d70a639017d1dd7c8d36daa2258063608688e449fddf40ffdd46f992a78'
URL="https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/$FILE"
mkdir -p "$BASE/downloads" "$BASE/hostlibs"
curl --fail --location --proto '=https' "$URL" --output "$BASE/downloads/$FILE"
printf '%s  %s\n' "$SHA" "$BASE/downloads/$FILE" | sha256sum --check --strict
dpkg-deb --extract "$BASE/downloads/$FILE" "$BASE/hostlibs"
printf 'Bibliotecas privadas del empaquetador: %s/hostlibs\n' "$BASE"
