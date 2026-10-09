# SPDX-License-Identifier: GPL-3.0-only
# Parámetros adaptados de los ejemplos hello_world/input de OpenOrbis v0.5.4.
ifeq ($(strip $(OO_PS4_TOOLCHAIN)),)
$(error Falta OO_PS4_TOOLCHAIN. Ejecuta source ~/.config/xcloud4/env.sh)
endif

SDK := $(OO_PS4_TOOLCHAIN)
X4_WEBRTC_ROOT ?= $(HOME)/.local/share/xcloud4/webrtc
X4_WEBRTC_PREFIX ?= $(X4_WEBRTC_ROOT)/prefix
CC := clang
LD := ld.lld
CFLAGS := --target=x86_64-pc-freebsd12-elf -std=c23 -fPIC -funwind-tables -O2 -g -Wall -Wextra -MMD -MP -c -isysroot $(SDK) -I$(X4_WEBRTC_ROOT)/overlay -I$(X4_WEBRTC_PREFIX)/include -isystem $(SDK)/include -D_BSD_SOURCE -DX4_OPENORBIS
LDFLAGS := -m elf_x86_64 -pie --script $(SDK)/link.x --eh-frame-hdr -L$(SDK)/lib
RTC_LIBS := $(X4_WEBRTC_ROOT)/build-datachannel/libdatachannel-static.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/libjuice/libjuice-static.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/usrsctp/usrsctplib/libusrsctp.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/libsrtp/libsrtp2.a \
    $(X4_WEBRTC_PREFIX)/lib/libmbedtls.a $(X4_WEBRTC_PREFIX)/lib/libmbedx509.a \
    $(X4_WEBRTC_PREFIX)/lib/libmbedcrypto.a $(X4_WEBRTC_PREFIX)/lib/libeverest.a \
    $(X4_WEBRTC_PREFIX)/lib/libp256m.a $(X4_WEBRTC_PREFIX)/lib/libopus.a
LIBS := -lScePosix -lc -lkernel -lSceVideoOut -lScePad -lSceUserService -lc++ -lc++abi -lunwind -lm
SOURCES := $(wildcard src/core/*.c src/ui/*.c src/video/*.c src/input/*.c src/audio/*.c src/auth/*.c src/streaming/*.c src/media/*.c)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))

.PHONY: all package
all: build/eboot.bin
package: build/eboot.bin
	bash scripts/empaquetar.sh

build:
	mkdir -p build

build/%.o: src/%.c Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

build/xcloud4.elf: $(OBJECTS) $(RTC_LIBS)
	$(LD) $(SDK)/lib/crt1.o $(OBJECTS) -o $@ $(LDFLAGS) --start-group $(RTC_LIBS) $(LIBS) --end-group

build/eboot.bin: build/xcloud4.elf
	$(SDK)/bin/linux/create-fself -in=$< -out=build/xcloud4.oelf --eboot $@ --paid 0x3800000000000011

-include $(OBJECTS:.o=.d)
