/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { X4_DIAG51_PATH_CAPACITY = 160 };

/* These diagnostic files contain only app and status data. Create a
 * non-followed, exclusive sibling with the ordinary 0666 file mode. */
FILE *x4_diag51_open(const char *path, char temporary[X4_DIAG51_PATH_CAPACITY], uint64_t stamp);
int x4_diag51_open_fd(const char *path, char temporary[X4_DIAG51_PATH_CAPACITY], uint64_t stamp);
bool x4_diag51_printf(FILE *file, const char *format, ...) __attribute__((format(printf, 2, 3)));
bool x4_diag51_puts(FILE *file, const char *text);
int x4_diag51_commit(FILE *file, const char *temporary, const char *path, bool complete);
int x4_diag51_commit_fd(int fd, const char *temporary, const char *path, bool complete);
