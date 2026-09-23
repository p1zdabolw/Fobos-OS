#include "window.h"
#include "font.h"
#include "../kernel/fb.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "compositor.h"

#define TITLE_H 22
#define BORDER  1

static struct window g_windows[MAX_WINDOWS];
static int g_count;
static int g_focus = -1;
static int g_dragging = -1;
static int g_drag_dx, g_drag_dy;
static int g_prev_left;

void window_init(void) {
    g_count = 0;
    g_focus = -1;
    g_dragging = -1;
    g_prev_left = 0;
}

int window_create(int x, int y, int w, int h, const char *title) {
    int id = -1;
    for (int i = 0; i < g_count; i++) {
        if (!g_windows[i].visible) { id = i; break; }
    }
    if (id < 0) {
        if (g_count >= MAX_WINDOWS) return -1;
        id = g_count++;
    }
    struct window *win = &g_windows[id];
    memset(win, 0, sizeof(*win));
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    strncpy(win->title, title, 31);
    win->title[31] = 0;
    win->visible = 1;
    g_focus = id;
    return id;
}

void window_destroy(int id) {
    if (id < 0 || id >= g_count) return;
    g_windows[id].visible = 0;
    if (g_focus == id) {
        g_focus = -1;
        for (int i = 0; i < g_count; i++) {
            if (g_windows[i].visible) { g_focus = i; break; }
        }
    }
    if (g_dragging == id) g_dragging = -1;
}

void window_focus(int id) {
    if (id < 0 || id >= g_count) return;
    g_focus = id;
}

int window_focused(void) { return g_focus; }
int window_count(void)   { return g_count; }
struct window *window_get(int id) {
    if (id < 0 || id >= g_count) return NULL;
    return &g_windows[id];
}

static int hit_test(struct window *win, int mx, int my, int *in_title, int *in_close) {
    if (mx < win->x || mx >= win->x + win->w) return 0;
    if (my < win->y || my >= win->y + win->h) return 0;
    *in_title = (my < win->y + TITLE_H);
    *in_close = (my < win->y + TITLE_H) && (mx >= win->x + win->w - 20) && (mx < win->x + win->w - 4);
    return 1;
}

static void draw_window(struct window *win, int focused) {
    if (!win->visible) return;
    u32 title_bg = focused ? 0x2A5A8C : 0x3A4753;
    u32 title_fg = focused ? 0xFFFFFF : 0xC8D4DE;
    u32 body_bg  = 0xE8E8E8;
    u32 border_c = focused ? 0x8FB8E0 : 0x2A3540;

    fb_fill_rect(win->x - BORDER, win->y - BORDER, win->w + 2*BORDER, win->h + 2*BORDER, border_c);
    fb_fill_rect(win->x, win->y, win->w, TITLE_H, title_bg);
    fb_fill_rect(win->x, win->y + TITLE_H, win->w, win->h - TITLE_H, body_bg);

    font_draw_string(win->x + 6, win->y + 3, win->title, title_fg, title_bg);

    int cx = win->x + win->w - 18;
    fb_fill_rect(cx, win->y + 5, 12, 12, 0xC05050);
    fb_fill_rect(cx + 2, win->y + 7, 8, 8, focused ? 0xE07070 : 0x9A4A4A);

    if (win->on_draw) win->on_draw(win);
}

void window_paint_all(void) {
    for (int i = 0; i < g_count; i++) {
        if (i == g_focus) continue;
        if (g_windows[i].visible) draw_window(&g_windows[i], 0);
    }
    if (g_focus >= 0 && g_focus < g_count && g_windows[g_focus].visible) {
        draw_window(&g_windows[g_focus], 1);
    }
}

void window_handle_mouse(int mx, int my, int left, int right) {
    (void)right;
    int new_left = left && !g_prev_left;
    int taskbar_y = (int)fb_get()->height - compositor_taskbar_h();

    if (new_left) {
        for (int i = g_count - 1; i >= 0; i--) {
            struct window *win = &g_windows[i];
            if (!win->visible) continue;
            int in_title = 0, in_close = 0;
            if (hit_test(win, mx, my, &in_title, &in_close)) {
                g_focus = i;
                if (in_close) {
                    window_destroy(i);
                } else if (in_title) {
                    g_dragging = i;
                    g_drag_dx = mx - win->x;
                    g_drag_dy = my - win->y;
                } else {
                    if (win->on_click) win->on_click(win, mx - win->x, my - win->y - TITLE_H);
                }
                break;
            }
        }
    }

    if (!left && g_prev_left) {
        g_dragging = -1;
    }

    if (left && g_dragging >= 0) {
        struct window *win = &g_windows[g_dragging];
        win->x = mx - g_drag_dx;
        win->y = my - g_drag_dy;
        if (win->x < 0) win->x = 0;
        if (win->y < 0) win->y = 0;
        if (win->x + win->w > (int)fb_get()->width) win->x = (int)fb_get()->width - win->w;
        if (win->y + win->h > taskbar_y) win->y = taskbar_y - win->h;
    }

    g_prev_left = left;
}

void window_handle_key(char c) {
    if (g_focus >= 0 && g_focus < g_count) {
        struct window *win = &g_windows[g_focus];
        if (win->visible && win->on_key) win->on_key(win, c);
    }
}

void window_handle_click(int x, int y) {
    (void)x;
    (void)y;
}

void window_sync_input(int left) {
    g_prev_left = left;
}