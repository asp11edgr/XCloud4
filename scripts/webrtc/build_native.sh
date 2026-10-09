#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
: "${OO_PS4_TOOLCHAIN:?Load ~/.config/xcloud4/env.sh first}"
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RTC_ROOT=${X4_WEBRTC_ROOT:-"$HOME/.local/share/xcloud4/webrtc"}
PREFIX="$RTC_ROOT/prefix"
mkdir -p "$RTC_ROOT" "$PREFIX/include" "$PREFIX/lib"

checkout_pinned() {
    local name=$1 url=$2 commit=$3
    if [[ ! -d "$RTC_ROOT/$name/.git" ]]; then
        git clone --no-checkout "$url" "$RTC_ROOT/$name"
        git -C "$RTC_ROOT/$name" checkout --detach "$commit"
        git -C "$RTC_ROOT/$name" submodule update --init --recursive
    fi
    [[ $(git -C "$RTC_ROOT/$name" rev-parse HEAD) == "$commit" ]] || {
        echo "Unexpected dependency commit: $name" >&2; exit 1;
    }
}
checkout_pinned libdatachannel https://github.com/paullouisageneau/libdatachannel.git bdc5ff28e9d3b863144c94a677ecf5bf043aaf15
checkout_pinned mbedtls https://github.com/Mbed-TLS/mbedtls.git 068ff080b369adfac81509f9b57b2afabaf82dc5
if [[ "$SCRIPT_DIR" != "$RTC_ROOT" ]]; then
    cp "$SCRIPT_DIR/openorbis.cmake" "$RTC_ROOT/openorbis.cmake"
    cp "$SCRIPT_DIR/mbedtls-user-config.h" "$RTC_ROOT/mbedtls-user-config.h"
fi
python3 "$SCRIPT_DIR/prepare_port.py" "$RTC_ROOT"

C_FLAGS="-fPIC -D_BSD_SOURCE -DX4_OPENORBIS -I$RTC_ROOT/overlay -isystem $OO_PS4_TOOLCHAIN/include"
CXX_FLAGS="$C_FLAGS -nostdinc++ -isystem $OO_PS4_TOOLCHAIN/include/c++/v1"
cmake -S "$RTC_ROOT/mbedtls" -B "$RTC_ROOT/build-mbedtls" \
    -DCMAKE_TOOLCHAIN_FILE="$RTC_ROOT/openorbis.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DMBEDTLS_USER_CONFIG_FILE="$RTC_ROOT/mbedtls-user-config.h" \
    -DCMAKE_C_FLAGS="$C_FLAGS"
cmake --build "$RTC_ROOT/build-mbedtls" -j1
cmake --install "$RTC_ROOT/build-mbedtls"

# Consumers must use exactly the library's profile, including struct layout
# changes from THREADING_C and the DTLS-SRTP declarations.
cp "$RTC_ROOT/mbedtls-user-config.h" "$PREFIX/include/x4-mbedtls-user-config.h"
if ! grep -Fq '#include "../x4-mbedtls-user-config.h"' "$PREFIX/include/mbedtls/mbedtls_config.h"; then
    printf '\n#include "../x4-mbedtls-user-config.h"\n' >> "$PREFIX/include/mbedtls/mbedtls_config.h"
fi

DNS_FLAGS='-Dgetaddrinfo=x4_native_getaddrinfo -Dfreeaddrinfo=x4_native_freeaddrinfo -Dgetnameinfo=x4_native_getnameinfo -Dgetifaddrs=x4_native_getifaddrs -Dfreeifaddrs=x4_native_freeifaddrs'
cmake -S "$RTC_ROOT/libdatachannel" -B "$RTC_ROOT/build-datachannel" \
    -DCMAKE_TOOLCHAIN_FILE="$RTC_ROOT/openorbis.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DUSE_MBEDTLS=ON -DNO_TESTS=ON \
    -DNO_EXAMPLES=ON -DNO_WEBSOCKET=ON -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_SHARED_DEPS_LIBS=OFF -DLIBSRTP_TEST_APPS=OFF \
    -Dsctp_inet=OFF -Dsctp_inet6=OFF -Dsctp_build_programs=OFF \
    -Dsctp_debug=OFF -Dsctp_werror=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$OO_PS4_TOOLCHAIN;$PREFIX" \
    -DMbedTLS_INCLUDE_DIR="$PREFIX/include" \
    -DMbedCrypto_LIBRARY="$PREFIX/lib/libmbedcrypto.a" \
    -DMbedTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
    -DMbedX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
    -DCMAKE_C_FLAGS="$C_FLAGS $DNS_FLAGS" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS $DNS_FLAGS"
cmake --build "$RTC_ROOT/build-datachannel" --target datachannel-static -j1
install -m644 "$RTC_ROOT/build-datachannel/libdatachannel-static.a" "$PREFIX/lib/"
install -m644 "$RTC_ROOT/build-datachannel/deps/libjuice/libjuice-static.a" "$PREFIX/lib/"
install -m644 "$RTC_ROOT/build-datachannel/deps/usrsctp/usrsctplib/libusrsctp.a" "$PREFIX/lib/"
install -m644 "$RTC_ROOT/build-datachannel/deps/libsrtp/libsrtp2.a" "$PREFIX/lib/"
cp -R "$RTC_ROOT/libdatachannel/include/rtc" "$PREFIX/include/"

# Opus is built from the official release archive whose hash was checked
# against the upstream release announcement. No programs or tests are built.
OPUS_ARCHIVE="$RTC_ROOT/opus-1.5.2.tar.gz"
OPUS_SHA256='65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'
if [[ ! -f "$OPUS_ARCHIVE" ]]; then
    curl --fail --location --proto '=https' --tlsv1.2 \
        --output "$OPUS_ARCHIVE.part" \
        https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
    printf '%s  %s\n' "$OPUS_SHA256" "$OPUS_ARCHIVE.part" | sha256sum --check -
    mv -- "$OPUS_ARCHIVE.part" "$OPUS_ARCHIVE"
fi
printf '%s  %s\n' "$OPUS_SHA256" "$OPUS_ARCHIVE" | sha256sum --check -
if [[ ! -d "$RTC_ROOT/opus-1.5.2" ]]; then
    tar -xzf "$OPUS_ARCHIVE" -C "$RTC_ROOT"
fi
cmake -S "$RTC_ROOT/opus-1.5.2" -B "$RTC_ROOT/build-opus" \
    -DCMAKE_TOOLCHAIN_FILE="$RTC_ROOT/openorbis.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_BUILD_TESTING=OFF -DOPUS_CUSTOM_MODES=OFF \
    -DOPUS_X86_MAY_HAVE_SSE=OFF -DOPUS_X86_MAY_HAVE_SSE2=OFF \
    -DOPUS_X86_MAY_HAVE_SSE4_1=OFF -DOPUS_X86_MAY_HAVE_AVX2=OFF
cmake --build "$RTC_ROOT/build-opus" -j1
cmake --install "$RTC_ROOT/build-opus"
