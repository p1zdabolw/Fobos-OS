#include "compositor.h"
#include "window.h"
#include "cursor.h"
#include "desktop.h"
#include "wallpaper.h"
#include "settings.h"
#include "../kernel/fb.h"
#include "../kernel/mouse.h"
#include "../kernel/timer.h"
#include "../kernel/keyboard.h"
#include "../kernel/rtc.h"
#include "../lib/i18n.h"
#include "font.h"
#include "../lib/printf.h"

#define TASKBAR_H 28
#define START_W 80
#define START_ITEMS 9
#define CTX_ITEMS 8
#define ITEM_H 24
#define MENU_PAD 4
#define TB_BTN_W 130
#define TB_BTN_GAP 4

static int g_menu_open;
static int g_ctx_open;
static int g_ctx_x;
static int g_ctx_y;
static int g_prev_left;
static int g_prev_right;

extern void apps_launch_terminal(void);
extern void apps_launch_files(void);
extern void apps_launch_notepad(void);
extern void apps_launch_about(void);
extern void apps_launch_browser(void);
extern void apps_launch_photos(void);
extern void apps_launch_media(void);

static const int g_start_ids[START_ITEMS] = {
    STR_APP_TERMINAL,
    STR_APP_BROWSER,
    STR_APP_NOTEPAD,
    STR_APP_FILES,
    STR_APP_PHOTOS,
    STR_APP_MEDIA,
    STR_APP_WALLPAPER,
    STR_APP_SETTINGS,
    STR_APP_ABOUT,
};

static const int g_ctx_ids[CTX_ITEMS] = {
    STR_CTX_NEW_TERMINAL,
    STR_CTX_NEW_BROWSER,
    STR_CTX_NEW_NOTEPAD,
    STR_CTX_NEW_FILES,
    STR_CTX_PHOTOS,
    STR_CTX_MEDIA,
    STR_CTX_WALLPAPER,
    STR_CTX_SETTINGS,
};

static int start_menu_h(void) { return 20 + START_ITEMS * ITEM_H + MENU_PAD; }
static int start_menu_y(void) { return (int)fb_get()->height - TASKBAR_H - start_menu_h(); }
static int ctx_menu_w(void)   { return 200; }
static int ctx_menu_h(void)   { return MENU_PAD * 2 + CTX_ITEMS * ITEM_H; }

static void draw_taskbar(void) {
    struct fb_info *fb = fb_get();
    int w = (int)fb->width;
    int y = (int)fb->height - TASKBAR_H;
    fb_fill_rect(0, y, w, TASKBAR_H, 0x25323B);
    fb_fill_rect(0, y, w, 1, 0x3C4A55);
    fb_fill_rect(0, y + TASKBAR_H - 1, w, 1, 0x101820);

    u32 start_c = g_menu_open ? 0x3C78B4 : 0x2E4859;
    fb_fill_rect(2, y + 2, START_W, TASKBAR_H - 5, start_c);
    font_draw_string(10, y + 6, "FOS", 0xFFFFFF, start_c);

    int bx = START_W + 8;
    for (int i = 0; i < window_count(); i++) {
        struct window *win = window_get(i);
        if (!win || !win->visible) continue;
        u32 c = (window_focused() == i) ? 0x4A5D6E : 0x36485A;
        fb_fill_rect(bx, y + 3, TB_BTN_W, TASKBAR_H - 7, c);
        font_draw_string(bx + 6, y + 7, win->title, 0xE0E8EE, c);
        fb_fill_rect(bx + TB_BTN_W - 16, y + 8, 12, 12, 0xC05050);
        font_draw_string(bx + TB_BTN_W - 15, y + 6, "x", 0xFFFFFF, 0xC05050);
        bx += TB_BTN_W + TB_BTN_GAP;
    }

    struct rtc_time tm;
    rtc_read(&tm);
    char clock[24];
    if (settings_clock_24h()) {
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d",
                 tm.hour, tm.minute, tm.second);
    } else {
        int h12 = tm.hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(clock, sizeof(clock), "%d:%02d:%02d %s",
                 h12, tm.minute, tm.second, tm.hour < 12 ? "AM" : "PM");
    }
    int cw = font_text_width(clock);
    int clock_x = w - cw - 16;
    fb_fill_rect(clock_x, y + 3, cw + 12, TASKBAR_H - 7, 0x1B262F);
    font_draw_string(clock_x + 6, y + 7, clock, 0xC8D4DE, 0x1B262F);

    const char *lang = keyboard_layout_name();
    int lw = font_text_width(lang);
    int lang_x = clock_x - lw - 16;
    fb_fill_rect(lang_x, y + 3, lw + 10, TASKBAR_H - 7, 0x1B262F);
    font_draw_string(lang_x + 5, y + 7, lang, 0xE0A040, 0x1B262F);
}

static void draw_start_menu(void) {
    int x = 2;
    int y = start_menu_y();
    int w = 220;
    int h = start_menu_h();
    fb_fill_rect(x, y, w, h, 0x2A3742);
    fb_fill_rect(x, y, w, 1, 0x546878);
    fb_fill_rect(x, y + h - 1, w, 1, 0x101820);
    font_draw_string(x + 10, y + 6, tr(STR_START_TITLE), 0x8FB8E0, 0x2A3742);
    for (int i = 0; i < START_ITEMS; i++) {
        int iy = y + 20 + i * ITEM_H;
        font_draw_string(x + 12, iy + 4, tr(g_start_ids[i]), 0xE8F0F6, 0x2A3742);
    }
}

static void draw_context_menu(void) {
    struct fb_info *fb = fb_get();
    int x = g_ctx_x;
    int y = g_ctx_y;
    int w = ctx_menu_w();
    int h = ctx_menu_h();
    if (x + w > (int)fb->width)  x = (int)fb->width - w;
    if (y + h > (int)fb->height - TASKBAR_H) y = (int)fb->height - TASKBAR_H - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_ctx_x = x;
    g_ctx_y = y;

    fb_fill_rect(x + 2, y + 2, w, h, 0x101820);
    fb_fill_rect(x, y, w, h, 0xD0D8DE);
    fb_fill_rect(x, y, w, 1, 0xFFFFFF);
    fb_fill_rect(x, y, 1, h, 0xFFFFFF);
    fb_fill_rect(x, y + h - 1, w, 1, 0x808890);
    fb_fill_rect(x + w - 1, y, 1, h, 0x808890);

    for (int i = 0; i < CTX_ITEMS; i++) {
        int iy = y + MENU_PAD + i * ITEM_H;
        font_draw_string(x + 12, iy + 4, tr(g_ctx_ids[i]), 0x101820, 0xD0D8DE);
    }
}

void compositor_init(void) {
    g_menu_open = 0;
    g_ctx_open = 0;
    g_prev_left = 0;
    g_prev_right = 0;
}

void compositor_paint(void) {
    struct fb_info *fb = fb_get();
    wallpaper_draw(0, 0, (int)fb->width, (int)fb->height);
    desktop_paint();
    window_paint_all();
    draw_taskbar();
    if (g_menu_open) draw_start_menu();
    if (g_ctx_open)  draw_context_menu();
    cursor_draw(mouse_x(), mouse_y());
    fb_present();
}

void compositor_mark_dirty(void) { }

void compositor_toggle_menu(void) {
    g_menu_open = !g_menu_open;
    if (g_menu_open) g_ctx_open = 0;
}

int  compositor_menu_open(void)  { return g_menu_open; }
void compositor_close_menu(void) { g_menu_open = 0; }
int  compositor_taskbar_h(void)  { return TASKBAR_H; }
int  compositor_start_w(void)    { return START_W; }

static void dispatch_start(int item) {
    if (item == 0) apps_launch_terminal();
    else if (item == 1) apps_launch_browser();
    else if (item == 2) apps_launch_notepad();
    else if (item == 3) apps_launch_files();
    else if (item == 4) apps_launch_photos();
    else if (item == 5) apps_launch_media();
    else if (item == 6) wallpaper_launch_settings();
    else if (item == 7) settings_launch();
    else if (item == 8) apps_launch_about();
}

static void dispatch_ctx(int item) {
    if (item == 0) apps_launch_terminal();
    else if (item == 1) apps_launch_browser();
    else if (item == 2) apps_launch_notepad();
    else if (item == 3) apps_launch_files();
    else if (item == 4) apps_launch_photos();
    else if (item == 5) apps_launch_media();
    else if (item == 6) wallpaper_launch_settings();
    else if (item == 7) settings_launch();
}

static int taskbar_hit(int mx, int my) {
    int taskbar_y = (int)fb_get()->height - TASKBAR_H;
    if (my < taskbar_y) return -1;
    int bx = START_W + 8;
    for (int i = 0; i < window_count(); i++) {
        struct window *win = window_get(i);
        if (!win || !win->visible) continue;
        if (mx >= bx && mx < bx + TB_BTN_W) return i;
        bx += TB_BTN_W + TB_BTN_GAP;
    }
    return -1;
}

static int taskbar_close_hit(int mx, int my) {
    int taskbar_y = (int)fb_get()->height - TASKBAR_H;
    if (my < taskbar_y) return -1;
    int bx = START_W + 8;
    for (int i = 0; i < window_count(); i++) {
        struct window *win = window_get(i);
        if (!win || !win->visible) continue;
        if (mx >= bx + TB_BTN_W - 20 && mx < bx + TB_BTN_W && my >= taskbar_y + 5) return i;
        bx += TB_BTN_W + TB_BTN_GAP;
    }
    return -1;
}

int compositor_handle_click(int mx, int my, int left, int right) {
    int taskbar_y = (int)fb_get()->height - TASKBAR_H;
    int new_right = right && !g_prev_right;
    int new_left  = left  && !g_prev_left;
    int result = 0;

    if (new_right) {
        int idx = taskbar_hit(mx, my);
        if (idx >= 0) {
            window_destroy(idx);
            g_ctx_open = 0;
            g_menu_open = 0;
            result = 1;
        } else if (my < taskbar_y) {
            g_ctx_open = 1;
            g_ctx_x = mx;
            g_ctx_y = my;
            g_menu_open = 0;
            result = 1;
        } else {
            g_ctx_open = 0;
        }
    }

    if (new_left) {
        if (g_ctx_open) {
            int x = g_ctx_x;
            int y = g_ctx_y;
            int w = ctx_menu_w();
            int h = ctx_menu_h();
            if (mx >= x && mx < x + w && my >= y && my < y + h) {
                int item = (my - y - MENU_PAD) / ITEM_H;
                if (item < 0) item = 0;
                if (item >= CTX_ITEMS) item = CTX_ITEMS - 1;
                dispatch_ctx(item);
            }
            g_ctx_open = 0;
            result = 1;
        } else if (my >= taskbar_y) {
            if (mx < START_W) {
                g_menu_open = !g_menu_open;
                g_ctx_open = 0;
                result = 1;
            } else {
                int close_idx = taskbar_close_hit(mx, my);
                if (close_idx >= 0) {
                    window_destroy(close_idx);
                } else {
                    int idx = taskbar_hit(mx, my);
                    if (idx >= 0) window_focus(idx);
                }
                result = 1;
            }
        } else if (g_menu_open) {
            int y = start_menu_y();
            int h = start_menu_h();
            if (mx >= 2 && mx < 222 && my >= y && my < y + h) {
                int item = (my - y - 20) / ITEM_H;
                if (item < 0) item = 0;
                if (item >= START_ITEMS) item = START_ITEMS - 1;
                dispatch_start(item);
            }
            g_menu_open = 0;
            result = 1;
        }
    }

    g_prev_left = left;
    g_prev_right = right;
    return result;
}