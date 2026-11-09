/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define X4_FONT_REGULAR  0
#define X4_FONT_SEMIBOLD 1
#define X4_FONT_BOLD     2

/* UI-thread-owned, fixed-budget precomputed alpha atlas. init accepts a file
 * or the containing directory. 0 = verified atlas loaded; -1 = smooth built-in
 * Regular fallback remains usable. close releases the optional heap atlas.
 * Neither rendering nor measuring allocates or reads files. */
int x4_font_init(const char *asset_path);
void x4_font_close(void);
int x4_font_is_fallback(void);
size_t x4_font_heap_bytes(void);
/* x/y is the top-left of a line box, rather than the font baseline. All
 * sizes are bounded to 8..96 pixels. Width includes the right ink overhang. */
void x4_text_px(uint32_t *pixels, int x, int y, int size, int semibold,
                const char *text, uint32_t color);
int x4_text_width_px(int size, int semibold, const char *text);
void x4_text_ellipsis_px(uint32_t *pixels, int x, int y, int size, int semibold,
                        const char *text, int max_width, uint32_t color);
/* Returns bytes written excluding NUL; sanitizes invalid UTF-8 with U+FFFD,
 * preserves complete codepoints, and truncates only after a whole word with
 * U+2026. Returns zero if no whole word fits. Newlines end this single-line
 * operation. Output is always NUL-terminated if cap > 0. */
size_t x4_text_fit_utf8(char *out, size_t cap, int size, int semibold,
                       const char *text, int max_width);
/* Decode one codepoint, consuming at least one byte of a nonempty string.
 * Invalid/overlong/surrogate/truncated encodings yield U+FFFD. */
uint32_t x4_utf8_next(const char **cursor);
