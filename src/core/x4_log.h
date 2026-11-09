/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef X4_LOG_H
#define X4_LOG_H

/* Write one bounded record directly to the process's stdout descriptor. */
int x4_log_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
