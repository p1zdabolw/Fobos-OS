#ifndef FOS_WINDOW_H
#define FOS_WINDOW_H

#include "../kernel/types.h"

#define MAX_WINDOWS 8

struct window {
    int x, y, w, h;
    char title[32];
    int visible;
    int focused;
    int minimized;
    void (*on_draw)(struct window *win);
    void (*on_key)(struct window *win, char c);
    void (*on_click)(struct window *win, int mx, int my);
    void *user;
};

void window_init(void);
int  window_create(int x, int y, int w, int h, const char *title);
void window_destroy(int id);
void window_focus(int id);
int  window_focused(void);
int  window_count(void);
struct window *window_get(int id);
void window_paint_all(void);
void window_handle_mouse(int x, int y, int left, int right);
void window_handle_key(char c);
void window_handle_click(int x, int y);
void window_sync_input(int left);
void window_reclamp(void);

#endif