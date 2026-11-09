/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdint.h>
#include <stdio.h>

/* Determine the length from the open descriptor, never from stat metadata. */
FILE *x4_update51_open_regular(const char *path, uint64_t *size, const char **failed_function);
int x4_update51_file_size(FILE *file, uint64_t *size);

/* Exact I/O, returning zero or a negative errno. */
int x4_update51_read_all(FILE *file, void *bytes, size_t count);
int x4_update51_write_all(FILE *file, const void *bytes, size_t count);
int x4_update51_expect_eof(FILE *file);
