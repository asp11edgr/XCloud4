/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
int x4_module_open(const char *name);
int x4_module_symbol(int handle, const char *name, void **address);
