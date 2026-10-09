/* SPDX-License-Identifier: GPL-3.0-only */
#include "canvas.h"
/* Original 5x7 uppercase bitmap alphabet; no external font asset required. */
static const struct { char c; uint8_t rows[7]; } glyphs[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {'.',{0,0,0,0,0,6,6}}, {':',{0,6,6,0,6,6,0}},
    {'-',{0,0,0,31,0,0,0}}, {'/',{1,1,2,4,8,16,16}},
    {'+',{0,4,4,31,4,4,0}}, {'(',{2,4,8,8,8,4,2}},
    {')',{8,4,2,2,2,4,8}}, {'?',{14,17,1,2,4,0,4}},
};
uint32_t x4_rgb(unsigned r, unsigned g, unsigned b)
{ return 0xff000000u | (b << 16) | (g << 8) | r; }
void x4_rect(uint32_t *p, int x, int y, int w, int h, uint32_t color)
{
    int right = x + w, bottom = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > X4_WIDTH) right = X4_WIDTH;
    if (bottom > X4_HEIGHT) bottom = X4_HEIGHT;
    for (int row = y; row < bottom; ++row)
        for (int col = x; col < right; ++col) p[row * X4_WIDTH + col] = color;
}
void x4_text(uint32_t *p, int x, int y, int scale, const char *text, uint32_t color)
{
    const int origin = x;
    if (scale < 1) return;
    for (; *text; ++text) {
        char c = *text;
        if (c == '\n') { x = origin; y += 10 * scale; continue; }
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        const uint8_t *rows = NULL;
        for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i)
            if (glyphs[i].c == c) { rows = glyphs[i].rows; break; }
        if (rows)
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (rows[row] & (1u << (4 - col)))
                        x4_rect(p, x + col * scale, y + row * scale, scale, scale, color);
        x += 6 * scale;
    }
}
