#include "widget.h"
#include "font.h"
#include "../kernel/fb.h"

int widget_point_in(struct widget_rect *r, int mx, int my) {
    return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

void widget_draw_button(struct widget_rect *r, const char *label, int pressed) {
    u32 base = pressed ? 0x2A5A8C : 0x4A6A8A;
    u32 top  = pressed ? 0x1B3F63 : 0x6A8AA8;
    u32 bot  = pressed ? 0x3C78B4 : 0x2A4056;
    fb_fill_rect(r->x, r->y, r->w, r->h, base);
    fb_fill_rect(r->x, r->y, r->w, 1, top);
    fb_fill_rect(r->x, r->y + r->h - 1, r->w, 1, bot);
    fb_fill_rect(r->x, r->y, 1, r->h, top);
    fb_fill_rect(r->x + r->w - 1, r->y, 1, r->h, bot);
    int tw = font_text_width(label);
    font_draw_string(r->x + (r->w - tw)/2, r->y + (r->h - FONT_H)/2, label, 0xFFFFFF, base);
}

void widget_draw_label(int x, int y, const char *text, u32 fg, u32 bg) {
    font_draw_string(x, y, text, fg, bg);
}

void widget_draw_textbox(struct widget_rect *r, const char *text, int focused) {
    fb_fill_rect(r->x, r->y, r->w, r->h, 0xFFFFFF);
    fb_fill_rect(r->x, r->y, r->w, 1, focused ? 0x3C78B4 : 0x808080);
    fb_fill_rect(r->x, r->y + r->h - 1, r->w, 1, focused ? 0x3C78B4 : 0x808080);
    fb_fill_rect(r->x, r->y, 1, r->h, focused ? 0x3C78B4 : 0x808080);
    fb_fill_rect(r->x + r->w - 1, r->y, 1, r->h, focused ? 0x3C78B4 : 0x808080);
    font_draw_string(r->x + 4, r->y + (r->h - FONT_H)/2, text, 0x101010, 0xFFFFFF);
}