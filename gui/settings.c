#include "settings.h"
#include "window.h"
#include "font.h"
#include "widget.h"
#include "wallpaper.h"
#include "../kernel/fb.h"
#include "../kernel/timer.h"
#include "../kernel/pmm.h"
#include "../kernel/net.h"
#include "../kernel/keyboard.h"
#include "../lib/i18n.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

extern int fb_resize(int w, int h);

#define SET_COUNT 6

static const int g_res_w[SET_COUNT] = { 640, 800, 1024, 1280, 1280, 1920 };
static const int g_res_h[SET_COUNT] = { 480, 600,  768,  720, 1024, 1080 };

#define SET_W 480
#define SET_H 680

#define X_OFF 12

#define Y_DISPLAY_TITLE   8
#define Y_DISPLAY_BTNS    30
#define Y_DISPLAY_CURRENT 96

#define Y_LANG_TITLE      130
#define Y_LANG_BTNS       154
#define Y_LANG_HINT       188
#define Y_LANG_LABEL      214
#define Y_LANG_FIELD      236
#define Y_LANG_FIELD_H    22
#define Y_LANG_LASTKEY    262

#define Y_UI_TITLE        292
#define Y_UI_BTNS         316
#define Y_UI_HINT         350

#define Y_WP_TITLE        380
#define Y_WP_BTN          404

#define Y_CLOCK_TITLE     444
#define Y_CLOCK_BTNS      468

#define Y_MOUSE_TITLE     508
#define Y_MOUSE_BTNS      532

#define Y_SYS_TITLE       572
#define Y_SYS_TEXT        596

static int g_clock_24h = 1;
static int g_mouse_speed = 1;
static char g_last_status[64] = "";

static char g_test_text[64] = "";
static int  g_test_len = 0;
static u8   g_last_key_byte = 0;
static int  g_key_count = 0;

void settings_init(void) {
    g_clock_24h = 1;
    g_mouse_speed = 1;
    g_last_status[0] = 0;
    g_test_text[0] = 0;
    g_test_len = 0;
    g_last_key_byte = 0;
    g_key_count = 0;
}

int  settings_clock_24h(void) { return g_clock_24h; }
void settings_set_clock_24h(int on) { g_clock_24h = on ? 1 : 0; }

int  settings_mouse_speed(void) { return g_mouse_speed; }
void settings_set_mouse_speed(int speed) {
    if (speed < 0) speed = 0;
    if (speed > 2) speed = 2;
    g_mouse_speed = speed;
}

static void draw_section(int x, int y, const char *title) {
    font_draw_string(x, y, title, 0x101820, 0xE8E8E8);
    fb_fill_rect(x, y + 16, 400, 1, 0x808890);
}

static void draw_button_colored(struct widget_rect *r, const char *label, int highlight) {
    u32 base   = highlight ? 0x3C78B4 : 0x4A6A8A;
    u32 top    = highlight ? 0x60A0E0 : 0x6A8AA8;
    u32 bottom = highlight ? 0x2A5A8C : 0x2A4056;
    fb_fill_rect(r->x, r->y, r->w, r->h, base);
    fb_fill_rect(r->x, r->y, r->w, 1, top);
    fb_fill_rect(r->x, r->y + r->h - 1, r->w, 1, bottom);
    fb_fill_rect(r->x, r->y, 1, r->h, top);
    fb_fill_rect(r->x + r->w - 1, r->y, 1, r->h, bottom);
    int tw = font_text_width(label);
    font_draw_string(r->x + (r->w - tw) / 2, r->y + (r->h - FONT_H) / 2, label, 0xFFFFFF, base);
}

static void draw_text_field(int x, int y, int w, int h, const char *text,
                            int show_cursor, int cursor_x) {
    fb_fill_rect(x, y, w, h, 0xFFFFFF);
    fb_fill_rect(x, y, w, 1, 0x3C78B4);
    fb_fill_rect(x, y + h - 1, w, 1, 0x3C78B4);
    fb_fill_rect(x, y, 1, h, 0x3C78B4);
    fb_fill_rect(x + w - 1, y, 1, h, 0x3C78B4);
    font_draw_string(x + 4, y + 3, text, 0x101010, 0xFFFFFF);
    if (show_cursor && ((timer_ticks() / 50) & 1)) {
        fb_fill_rect(x + 4 + cursor_x * FONT_W, y + 3, FONT_W, FONT_H, 0x303030);
    }
}

static void set_draw(struct window *win) {
    struct fb_info *fb = fb_get();
    int bx = win->x + X_OFF;
    int by = win->y + 22;

    draw_section(bx, by + Y_DISPLAY_TITLE, tr(STR_SETTINGS_DISPLAY));

    for (int i = 0; i < SET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        struct widget_rect r;
        r.x = bx + col * 132;
        r.y = by + Y_DISPLAY_BTNS + row * 32;
        r.w = 126;
        r.h = 26;
        char buf[24];
        snprintf(buf, sizeof(buf), "%dx%d", g_res_w[i], g_res_h[i]);
        int hl = ((int)fb->width == g_res_w[i] && (int)fb->height == g_res_h[i]);
        draw_button_colored(&r, buf, hl);
    }

    {
        char cur[80];
        snprintf(cur, sizeof(cur), tr(STR_SETTINGS_CURRENT), fb->width, fb->height);
        font_draw_string(bx, by + Y_DISPLAY_CURRENT, cur, 0x303840, 0xE8E8E8);
    }
    if (g_last_status[0]) {
        font_draw_string(bx, by + Y_DISPLAY_CURRENT + 16, g_last_status, 0x902010, 0xE8E8E8);
    }

    draw_section(bx, by + Y_LANG_TITLE, tr(STR_SETTINGS_LANG));

    struct widget_rect b_en, b_ru;
    b_en.x = bx;        b_en.y = by + Y_LANG_BTNS; b_en.w = 100; b_en.h = 26;
    b_ru.x = bx + 110;  b_ru.y = by + Y_LANG_BTNS; b_ru.w = 120; b_ru.h = 26;
    draw_button_colored(&b_en, "English", keyboard_get_layout() == LAYOUT_EN);
    draw_button_colored(&b_ru, "\xD0\xF3\xF1\xF1\xEA\xE8\xE9",
                        keyboard_get_layout() == LAYOUT_RU);

    font_draw_string(bx, by + Y_LANG_HINT, tr(STR_SETTINGS_LANG_HINT),
                     0x303840, 0xE8E8E8);

    font_draw_string(bx, by + Y_LANG_LABEL, tr(STR_SETTINGS_KB_TEST),
                     0x303840, 0xE8E8E8);

    draw_text_field(bx, by + Y_LANG_FIELD, 340, Y_LANG_FIELD_H,
                    g_test_text, 1, g_test_len);
    {
        struct widget_rect b_clr;
        b_clr.x = bx + 348; b_clr.y = by + Y_LANG_FIELD;
        b_clr.w = 76; b_clr.h = Y_LANG_FIELD_H;
        draw_button_colored(&b_clr, tr(STR_SETTINGS_CLEAR), 0);
    }

    {
        char diag[80];
        snprintf(diag, sizeof(diag), tr(STR_SETTINGS_LASTKEY),
                 (u32)g_last_key_byte, g_key_count);
        font_draw_string(bx, by + Y_LANG_LASTKEY, diag, 0x606870, 0xE8E8E8);
    }

    draw_section(bx, by + Y_UI_TITLE, tr(STR_SETTINGS_UI_LANG));
    {
        struct widget_rect b_ui_en, b_ui_ru;
        b_ui_en.x = bx;        b_ui_en.y = by + Y_UI_BTNS; b_ui_en.w = 110; b_ui_en.h = 26;
        b_ui_ru.x = bx + 120;  b_ui_ru.y = by + Y_UI_BTNS; b_ui_ru.w = 130; b_ui_ru.h = 26;
        draw_button_colored(&b_ui_en, "English", i18n_get() == LANG_EN);
        draw_button_colored(&b_ui_ru, "\xD0\xF3\xF1\xF1\xEA\xE8\xE9",
                            i18n_get() == LANG_RU);
        font_draw_string(bx, by + Y_UI_HINT, tr(STR_SETTINGS_UI_HINT),
                         0x606870, 0xE8E8E8);
    }

    draw_section(bx, by + Y_WP_TITLE, tr(STR_SETTINGS_WALLPAPER));
    {
        struct widget_rect b_wp;
        b_wp.x = bx; b_wp.y = by + Y_WP_BTN; b_wp.w = 280; b_wp.h = 26;
        char buf[80];
        snprintf(buf, sizeof(buf), tr(STR_SETTINGS_OPEN_WP),
                 wallpaper_name(wallpaper_get()));
        draw_button_colored(&b_wp, buf, 0);
    }

    draw_section(bx, by + Y_CLOCK_TITLE, tr(STR_SETTINGS_CLOCK));
    {
        struct widget_rect b12, b24;
        b12.x = bx;        b12.y = by + Y_CLOCK_BTNS; b12.w = 110; b12.h = 26;
        b24.x = bx + 120;  b24.y = by + Y_CLOCK_BTNS; b24.w = 110; b24.h = 26;
        draw_button_colored(&b12, tr(STR_SETTINGS_12H), !g_clock_24h);
        draw_button_colored(&b24, tr(STR_SETTINGS_24H),  g_clock_24h);
    }

    draw_section(bx, by + Y_MOUSE_TITLE, tr(STR_SETTINGS_MOUSE));
    {
        struct widget_rect bslow, bnorm, bfast;
        bslow.x = bx;        bslow.y = by + Y_MOUSE_BTNS; bslow.w = 100; bslow.h = 26;
        bnorm.x = bx + 110;  bnorm.y = by + Y_MOUSE_BTNS; bnorm.w = 100; bnorm.h = 26;
        bfast.x = bx + 220;  bfast.y = by + Y_MOUSE_BTNS; bfast.w = 100; bfast.h = 26;
        draw_button_colored(&bslow, tr(STR_SETTINGS_SLOW),   g_mouse_speed == 0);
        draw_button_colored(&bnorm, tr(STR_SETTINGS_NORMAL), g_mouse_speed == 1);
        draw_button_colored(&bfast, tr(STR_SETTINGS_FAST),   g_mouse_speed == 2);
    }

    draw_section(bx, by + Y_SYS_TITLE, tr(STR_SETTINGS_SYSINFO));
    {
        char buf[96];
        int y = by + Y_SYS_TEXT;
        extern u64 pmm_free_bytes(void);
        extern u64 pmm_total_bytes(void);
        snprintf(buf, sizeof(buf), tr(STR_SETTINGS_MEMORY),
                 (u32)(pmm_free_bytes() / 1024), (u32)(pmm_total_bytes() / 1024));
        font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
        y += 16;
        u64 secs = timer_uptime_seconds();
        snprintf(buf, sizeof(buf), tr(STR_SETTINGS_UPTIME),
                 (u32)(secs / 3600), (u32)((secs / 60) % 60), (u32)(secs % 60));
        font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
        y += 16;
        if (net_ready()) {
            u32 ip = net_our_ip();
            snprintf(buf, sizeof(buf), tr(STR_SETTINGS_NETWORK),
                     (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        } else {
            snprintf(buf, sizeof(buf), "%s", tr(STR_SETTINGS_NO_NET));
        }
        font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
        y += 16;
        font_draw_string(bx, y, tr(STR_SETTINGS_KERNEL), 0x303840, 0xE8E8E8);
    }
}

static int do_resize(int w, int h) {
    int r = fb_resize(w, h);
    if (r != 0) {
        strncpy(g_last_status, tr(STR_SETTINGS_RES_FAIL), sizeof(g_last_status) - 1);
        g_last_status[sizeof(g_last_status) - 1] = 0;
        return -1;
    }
    extern void window_reclamp(void);
    extern void desktop_reclamp(void);
    window_reclamp();
    desktop_reclamp();
    g_last_status[0] = 0;
    return 0;
}

static int hit(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void set_click(struct window *win, int x, int y) {
    (void)win;
    int bx = X_OFF;

    for (int i = 0; i < SET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int rx = bx + col * 132;
        int ry = Y_DISPLAY_BTNS + row * 32;
        if (hit(x, y, rx, ry, 126, 26)) {
            do_resize(g_res_w[i], g_res_h[i]);
            return;
        }
    }

    if (hit(x, y, bx, Y_LANG_BTNS, 100, 26)) {
        keyboard_set_layout(LAYOUT_EN);
        return;
    }
    if (hit(x, y, bx + 110, Y_LANG_BTNS, 120, 26)) {
        keyboard_set_layout(LAYOUT_RU);
        return;
    }
    if (hit(x, y, bx + 348, Y_LANG_FIELD, 76, Y_LANG_FIELD_H)) {
        g_test_len = 0;
        g_test_text[0] = 0;
        return;
    }

    if (hit(x, y, bx, Y_UI_BTNS, 110, 26)) {
        i18n_set(LANG_EN);
        return;
    }
    if (hit(x, y, bx + 120, Y_UI_BTNS, 130, 26)) {
        i18n_set(LANG_RU);
        return;
    }

    if (hit(x, y, bx, Y_WP_BTN, 280, 26)) {
        extern void wallpaper_launch_settings(void);
        wallpaper_launch_settings();
        return;
    }

    if (hit(x, y, bx, Y_CLOCK_BTNS, 110, 26)) {
        g_clock_24h = 0;
        return;
    }
    if (hit(x, y, bx + 120, Y_CLOCK_BTNS, 110, 26)) {
        g_clock_24h = 1;
        return;
    }

    if (hit(x, y, bx, Y_MOUSE_BTNS, 100, 26)) {
        g_mouse_speed = 0;
        return;
    }
    if (hit(x, y, bx + 110, Y_MOUSE_BTNS, 100, 26)) {
        g_mouse_speed = 1;
        return;
    }
    if (hit(x, y, bx + 220, Y_MOUSE_BTNS, 100, 26)) {
        g_mouse_speed = 2;
        return;
    }
}

static void set_key(struct window *win, char c) {
    (void)win;
    u8 u = (u8)c;
    g_last_key_byte = u;
    g_key_count++;

    if (u == 0x0E) {
        if (g_test_len > 0) {
            g_test_len--;
            g_test_text[g_test_len] = 0;
        }
        return;
    }
    if (u == 0x1C || u == '\n') {
        g_test_len = 0;
        g_test_text[0] = 0;
        return;
    }
    if ((u >= 32 && u < 127) || u >= 0xC0) {
        if (g_test_len < 40) {
            g_test_text[g_test_len++] = (char)u;
            g_test_text[g_test_len] = 0;
        }
    }
}

static struct window *g_settings_win = 0;

void settings_launch(void) {
    for (int i = 0; i < window_count(); i++) {
        struct window *w = window_get(i);
        if (w && w->visible && w->on_draw == set_draw) {
            window_focus(i);
            return;
        }
    }
    int id = window_create(100, 20, SET_W, SET_H, tr(STR_APP_SETTINGS));
    if (id < 0) return;
    g_settings_win = window_get(id);
    g_settings_win->on_draw = set_draw;
    g_settings_win->on_click = set_click;
    g_settings_win->on_key = set_key;
}