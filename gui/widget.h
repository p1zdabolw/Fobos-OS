#ifndef FOS_WIDGET_H
#define FOS_WIDGET_H

#include "../kernel/types.h"

struct widget_rect { int x, y, w, h; };

int  widget_point_in(struct widget_rect *r, int mx, int my);
void widget_draw_button(struct widget_rect *r, const char *label, int pressed);
void widget_draw_label(int x, int y, const char *text, u32 fg, u32 bg);
void widget_draw_textbox(struct widget_rect *r, const char *text, int focused);

#endif