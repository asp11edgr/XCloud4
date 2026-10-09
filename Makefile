# SPDX-License-Identifier: GPL-3.0-only
# Parámetros adaptados de los ejemplos hello_world/input de OpenOrbis v0.5.4.
ifeq ($(strip $(OO_PS4_TOOLCHAIN)),)
$(error Falta OO_PS4_TOOLCHAIN. Ejecuta source ~/.config/xcloud4/env.sh)
endif

SDK := $(OO_PS4_TOOLCHAIN)
CC := clang
LD := ld.lld
CFLAGS := --target=x86_64-pc-freebsd12-elf -std=c23 -fPIC -funwind-tables -O2 -g -Wall -Wextra -MMD -MP -c -isysroot $(SDK) -isystem $(SDK)/include
LDFLAGS := -m elf_x86_64 -pie --script $(SDK)/link.x --eh-frame-hdr -L$(SDK)/lib
LIBS := -lc -lkernel -lSceVideoOut -lScePad -lSceUserService
SOURCES := $(wildcard src/core/*.c src/ui/*.c src/video/*.c src/input/*.c)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))

.PHONY: all package
all: build/eboot.bin
package: build/eboot.bin
	bash scripts/empaquetar.sh

build:
	mkdir -p build

build/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

build/xcloud4.elf: $(OBJECTS)
	$(LD) $(SDK)/lib/crt1.o $(OBJECTS) -o $@ $(LDFLAGS) $(LIBS)

build/eboot.bin: build/xcloud4.elf
	$(SDK)/bin/linux/create-fself -in=$< -out=build/xcloud4.oelf --eboot $@ --paid 0x3800000000000011

-include $(OBJECTS:.o=.d)
