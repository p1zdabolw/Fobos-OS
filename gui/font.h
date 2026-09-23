#ifndef FOS_FONT_H
#define FOS_FONT_H

#include "../kernel/types.h"

#define FONT_W 8
#define FONT_H 16

void font_init(void);
void font_draw_char(int x, int y, char c, u32 fg, u32 bg);
void font_draw_string(int x, int y, const char *s, u32 fg, u32 bg);
void font_draw_string_alpha(int x, int y, const char *s, u32 fg, u32 bg);
int  font_text_width(const char *s);

#endif