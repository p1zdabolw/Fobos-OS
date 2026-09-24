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
#include "../kernel/rtc.h"
#include "../kernel/fs.h"
#include "../lib/i18n.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

extern int fb_resize(int w, int h);

#define SET_COUNT 6
#define SETTINGS_MAGIC   0x53455453u
#define SETTINGS_FILE    "settings.bin"

struct settings_blob {
    u32 magic;
    u32 ui_lang;
    u32 kbd_lang;
    u32 clock24;
    u32 mouse_speed;
    u32 wallpaper_mode;
    u32 photo_index;
    u32 tz_offset_min;
    u32 reserved[7];
    u32 checksum;
};

struct tz_entry {
    const char *label;
    int offset_min;
};

static const struct tz_entry TZ_LIST[] = {
    { "UTC-12:00",             -720 },
    { "UTC-11:00",             -660 },
    { "UTC-10:00 Hawaii",      -600 },
    { "UTC-09:00 Alaska",      -540 },
    { "UTC-08:00 Los Angeles", -480 },
    { "UTC-07:00 Denver",      -420 },
    { "UTC-06:00 Chicago",     -360 },
    { "UTC-05:00 New York",    -300 },
    { "UTC-04:00 Santiago",    -240 },
    { "UTC-03:00 Sao Paulo",   -180 },
    { "UTC-02:00",             -120 },
    { "UTC-01:00 Azores",       -60 },
    { "UTC+00:00 London",         0 },
    { "UTC+01:00 Berlin",        60 },
    { "UTC+02:00 Cairo",        120 },
    { "UTC+03:00 Moscow",       180 },
    { "UTC+04:00 Dubai",        240 },
    { "UTC+05:00 Karachi",      300 },
    { "UTC+05:30 Mumbai",       330 },
    { "UTC+06:00 Dhaka",        360 },
    { "UTC+07:00 Bangkok",      420 },
    { "UTC+08:00 Beijing",      480 },
    { "UTC+09:00 Tokyo",        540 },
    { "UTC+10:00 Sydney",       600 },
    { "UTC+11:00",              660 },
    { "UTC+12:00 Auckland",     720 },
    { "UTC+13:00",              780 },
    { "UTC+14:00",              840 },
};
#define TZ_COUNT ((int)(sizeof(TZ_LIST) / sizeof(TZ_LIST[0])))

static const int g_res_w[SET_COUNT] = { 640, 800, 1024, 1280, 1280, 1920 };
static const int g_res_h[SET_COUNT] = { 480, 600,  768,  720, 1024, 1080 };

#define SET_W 480
#define SET_H 780

#define X_OFF 12

#define Y_DISPLAY_TITLE   8
#define Y_DISPLAY_BTNS    30
#define Y_DISPLAY_CURRENT 96

#define Y_LANG_TITLE      130
#define Y_LANG_BTNS       154
#define Y_LANG_HINT       188

#define Y_UI_TITLE        214
#define Y_UI_BTNS         238
#define Y_UI_HINT         272

#define Y_TZ_TITLE        300
#define Y_TZ_BTNS         324
#define Y_TZ_HINT         360

#define Y_WP_TITLE        388
#define Y_WP_BTN          412

#define Y_CLOCK_TITLE     452
#define Y_CLOCK_BTNS      476

#define Y_MOUSE_TITLE     516
#define Y_MOUSE_BTNS      540

#define Y_SYS_TITLE       580
#define Y_SYS_TEXT        604

static int g_clock_24h = 1;
static int g_mouse_speed = 1;
static char g_last_status[64] = "";

static int tz_find_index(int offset) {
    for (int i = 0; i < TZ_COUNT; i++) {
        if (TZ_LIST[i].offset_min == offset) return i;
    }
    return -1;
}

static u32 blob_checksum(const struct settings_blob *b) {
    u32 h = 0x811c9dc5u;
    const u8 *p = (const u8*)b;
    for (usize i = 0; i < sizeof(*b) - sizeof(u32); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void settings_save(void) {
    struct settings_blob b;
    memset(&b, 0, sizeof(b));
    b.magic = SETTINGS_MAGIC;
    b.ui_lang = (u32)i18n_get();
    b.kbd_lang = (u32)keyboard_get_layout();
    b.clock24 = (u32)g_clock_24h;
    b.mouse_speed = (u32)g_mouse_speed;
    b.wallpaper_mode = (u32)wallpaper_get();
    b.photo_index = (u32)wallpaper_photo_index();
    b.tz_offset_min = (u32)rtc_get_offset();
    b.checksum = blob_checksum(&b);
    fs_write(SETTINGS_FILE, &b, sizeof(b));
    fs_sync();
}

void settings_load(void) {
    usize sz = 0;
    void *p = fs_read(SETTINGS_FILE, &sz);
    if (!p || sz < sizeof(struct settings_blob)) return;
    struct settings_blob b;
    memcpy(&b, p, sizeof(b));
    if (b.magic != SETTINGS_MAGIC) return;
    if (b.checksum != blob_checksum(&b)) return;

    i18n_set((int)b.ui_lang);
    keyboard_set_layout((int)b.kbd_lang);
    g_clock_24h = b.clock24 ? 1 : 0;
    g_mouse_speed = (int)b.mouse_speed;
    rtc_set_offset((int)b.tz_offset_min);
    if (b.wallpaper_mode == WALL_PHOTO) {
        wallpaper_set_photo((int)b.photo_index);
    } else {
        wallpaper_set((int)b.wallpaper_mode);
    }
}

void settings_init(void) {
    g_clock_24h = 1;
    g_mouse_speed = 1;
    g_last_status[0] = 0;
    rtc_set_offset(0);
}

int  settings_clock_24h(void) { return g_clock_24h; }
void settings_set_clock_24h(int on) { g_clock_24h = on ? 1 : 0; }

int  settings_mouse_speed(void) { return g_mouse_speed; }
void settings_set_mouse_speed(int speed) {
    if (speed < 0) speed = 0;
    if (speed > 2) speed = 2;
    g_mouse_speed = speed;
}

static int content_fits(struct window *win, int y) {
    int avail_h = win->h - 22;
    return y < avail_h - 4;
}

static void draw_section(struct window *win, int x, int y, const char *title) {
    if (!content_fits(win, y + 16)) return;
    font_draw_string(x, y, title, 0x101820, 0xE8E8E8);
    int line_w = win->w - 2 * X_OFF;
    if (line_w > 400) line_w = 400;
    if (line_w < 0) line_w = 0;
    fb_fill_rect(x, y + 16, line_w, 1, 0x808890);
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

static void set_draw(struct window *win) {
    struct fb_info *fb = fb_get();
    int bx = win->x + X_OFF;
    int by = win->y + 22;

    draw_section(win, bx, by + Y_DISPLAY_TITLE, tr(STR_SETTINGS_DISPLAY));

    for (int i = 0; i < SET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int rel_y = Y_DISPLAY_BTNS + row * 32;
        if (!content_fits(win, rel_y + 26)) continue;
        struct widget_rect r;
        r.x = bx + col * 132;
        r.y = by + rel_y;
        r.w = 126;
        r.h = 26;
        char buf[24];
        snprintf(buf, sizeof(buf), "%dx%d", g_res_w[i], g_res_h[i]);
        int hl = ((int)fb->width == g_res_w[i] && (int)fb->height == g_res_h[i]);
        draw_button_colored(&r, buf, hl);
    }

    if (content_fits(win, Y_DISPLAY_CURRENT)) {
        char cur[80];
        snprintf(cur, sizeof(cur), tr(STR_SETTINGS_CURRENT), fb->width, fb->height);
        font_draw_string(bx, by + Y_DISPLAY_CURRENT, cur, 0x303840, 0xE8E8E8);
    }
    if (g_last_status[0] && content_fits(win, Y_DISPLAY_CURRENT + 16)) {
        font_draw_string(bx, by + Y_DISPLAY_CURRENT + 16, g_last_status, 0x902010, 0xE8E8E8);
    }

    draw_section(win, bx, by + Y_LANG_TITLE, tr(STR_SETTINGS_LANG));
    if (content_fits(win, Y_LANG_BTNS + 26)) {
        struct widget_rect b_en, b_ru;
        b_en.x = bx;        b_en.y = by + Y_LANG_BTNS; b_en.w = 100; b_en.h = 26;
        b_ru.x = bx + 110;  b_ru.y = by + Y_LANG_BTNS; b_ru.w = 120; b_ru.h = 26;
        draw_button_colored(&b_en, "English", keyboard_get_layout() == LAYOUT_EN);
        draw_button_colored(&b_ru, "\xD0\xF3\xF1\xF1\xEA\xE8\xE9",
                            keyboard_get_layout() == LAYOUT_RU);
    }
    if (content_fits(win, Y_LANG_HINT + 16)) {
        font_draw_string(bx, by + Y_LANG_HINT, tr(STR_SETTINGS_LANG_HINT),
                         0x303840, 0xE8E8E8);
    }

    draw_section(win, bx, by + Y_UI_TITLE, tr(STR_SETTINGS_UI_LANG));
    if (content_fits(win, Y_UI_BTNS + 26)) {
        struct widget_rect b_ui_en, b_ui_ru;
        b_ui_en.x = bx;        b_ui_en.y = by + Y_UI_BTNS; b_ui_en.w = 110; b_ui_en.h = 26;
        b_ui_ru.x = bx + 120;  b_ui_ru.y = by + Y_UI_BTNS; b_ui_ru.w = 130; b_ui_ru.h = 26;
        draw_button_colored(&b_ui_en, "English", i18n_get() == LANG_EN);
        draw_button_colored(&b_ui_ru, "\xD0\xF3\xF1\xF1\xEA\xE8\xE9",
                            i18n_get() == LANG_RU);
    }
    if (content_fits(win, Y_UI_HINT + 16)) {
        font_draw_string(bx, by + Y_UI_HINT, tr(STR_SETTINGS_UI_HINT),
                         0x606870, 0xE8E8E8);
    }

    draw_section(win, bx, by + Y_TZ_TITLE, tr(STR_SETTINGS_TIMEZONE));
    if (content_fits(win, Y_TZ_BTNS + 26)) {
        struct widget_rect b_prev, b_next;
        b_prev.x = bx;        b_prev.y = by + Y_TZ_BTNS; b_prev.w = 32; b_prev.h = 26;
        b_next.x = bx + 388;  b_next.y = by + Y_TZ_BTNS; b_next.w = 32; b_next.h = 26;
        draw_button_colored(&b_prev, "<", 0);
        draw_button_colored(&b_next, ">", 0);

        int off = rtc_get_offset();
        int idx = tz_find_index(off);
        char label[40];
        if (idx >= 0) {
            strncpy(label, TZ_LIST[idx].label, sizeof(label) - 1);
            label[sizeof(label) - 1] = 0;
        } else {
            int sign = off < 0 ? '-' : '+';
            int a = off < 0 ? -off : off;
            snprintf(label, sizeof(label), "UTC%c%02d:%02d", sign, a / 60, a % 60);
        }

        struct widget_rect r;
        r.x = bx + 40; r.y = by + Y_TZ_BTNS; r.w = 340; r.h = 26;
        fb_fill_rect(r.x, r.y, r.w, r.h, 0xFFFFFF);
        fb_fill_rect(r.x, r.y, r.w, 1, 0x808890);
        fb_fill_rect(r.x, r.y + r.h - 1, r.w, 1, 0x808890);
        fb_fill_rect(r.x, r.y, 1, r.h, 0x808890);
        fb_fill_rect(r.x + r.w - 1, r.y, 1, r.h, 0x808890);
        int tw = font_text_width(label);
        font_draw_string(r.x + (r.w - tw) / 2, r.y + (r.h - FONT_H) / 2, label, 0x101010, 0xFFFFFF);
    }
    if (content_fits(win, Y_TZ_HINT + 16)) {
        font_draw_string(bx, by + Y_TZ_HINT,
                         "Clock shows RTC time plus this offset",
                         0x606870, 0xE8E8E8);
        font_draw_string(bx, by + Y_TZ_HINT + 14,
                         "QEMU RTC is host local; leave at UTC+00",
                         0x808890, 0xE8E8E8);
    }

    draw_section(win, bx, by + Y_WP_TITLE, tr(STR_SETTINGS_WALLPAPER));
    if (content_fits(win, Y_WP_BTN + 26)) {
        struct widget_rect b_wp;
        b_wp.x = bx; b_wp.y = by + Y_WP_BTN; b_wp.w = 280; b_wp.h = 26;
        char buf[80];
        snprintf(buf, sizeof(buf), tr(STR_SETTINGS_OPEN_WP),
                 wallpaper_name(wallpaper_get()));
        draw_button_colored(&b_wp, buf, 0);
    }

    draw_section(win, bx, by + Y_CLOCK_TITLE, tr(STR_SETTINGS_CLOCK));
    if (content_fits(win, Y_CLOCK_BTNS + 26)) {
        struct widget_rect b12, b24;
        b12.x = bx;        b12.y = by + Y_CLOCK_BTNS; b12.w = 110; b12.h = 26;
        b24.x = bx + 120;  b24.y = by + Y_CLOCK_BTNS; b24.w = 110; b24.h = 26;
        draw_button_colored(&b12, tr(STR_SETTINGS_12H), !g_clock_24h);
        draw_button_colored(&b24, tr(STR_SETTINGS_24H),  g_clock_24h);
    }

    draw_section(win, bx, by + Y_MOUSE_TITLE, tr(STR_SETTINGS_MOUSE));
    if (content_fits(win, Y_MOUSE_BTNS + 26)) {
        struct widget_rect bslow, bnorm, bfast;
        bslow.x = bx;        bslow.y = by + Y_MOUSE_BTNS; bslow.w = 100; bslow.h = 26;
        bnorm.x = bx + 110;  bnorm.y = by + Y_MOUSE_BTNS; bnorm.w = 100; bnorm.h = 26;
        bfast.x = bx + 220;  bfast.y = by + Y_MOUSE_BTNS; bfast.w = 100; bfast.h = 26;
        draw_button_colored(&bslow, tr(STR_SETTINGS_SLOW),   g_mouse_speed == 0);
        draw_button_colored(&bnorm, tr(STR_SETTINGS_NORMAL), g_mouse_speed == 1);
        draw_button_colored(&bfast, tr(STR_SETTINGS_FAST),   g_mouse_speed == 2);
    }

    draw_section(win, bx, by + Y_SYS_TITLE, tr(STR_SETTINGS_SYSINFO));
    if (content_fits(win, Y_SYS_TEXT)) {
        char buf[96];
        int y = by + Y_SYS_TEXT;
        extern u64 pmm_free_bytes(void);
        extern u64 pmm_total_bytes(void);
        snprintf(buf, sizeof(buf), tr(STR_SETTINGS_MEMORY),
                 (u32)(pmm_free_bytes() / 1024), (u32)(pmm_total_bytes() / 1024));
        font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
        y += 16;
        if (content_fits(win, y - by)) {
            u64 secs = timer_uptime_seconds();
            snprintf(buf, sizeof(buf), tr(STR_SETTINGS_UPTIME),
                     (u32)(secs / 3600), (u32)((secs / 60) % 60), (u32)(secs % 60));
            font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
            y += 16;
        }
        if (content_fits(win, y - by)) {
            if (net_ready()) {
                u32 ip = net_our_ip();
                snprintf(buf, sizeof(buf), tr(STR_SETTINGS_NETWORK),
                         (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
            } else {
                snprintf(buf, sizeof(buf), "%s", tr(STR_SETTINGS_NO_NET));
            }
            font_draw_string(bx, y, buf, 0x303840, 0xE8E8E8);
            y += 16;
        }
        if (content_fits(win, y - by)) {
            font_draw_string(bx, y, tr(STR_SETTINGS_KERNEL), 0x303840, 0xE8E8E8);
        }
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
    int bx = X_OFF;
    int avail_h = win->h - 22;

    for (int i = 0; i < SET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int ry = Y_DISPLAY_BTNS + row * 32;
        if (ry + 26 > avail_h) continue;
        int rx = bx + col * 132;
        if (hit(x, y, rx, ry, 126, 26)) {
            do_resize(g_res_w[i], g_res_h[i]);
            settings_save();
            return;
        }
    }

    if (Y_LANG_BTNS + 26 <= avail_h) {
        if (hit(x, y, bx, Y_LANG_BTNS, 100, 26)) {
            keyboard_set_layout(LAYOUT_EN); settings_save(); return;
        }
        if (hit(x, y, bx + 110, Y_LANG_BTNS, 120, 26)) {
            keyboard_set_layout(LAYOUT_RU); settings_save(); return;
        }
    }
    if (Y_UI_BTNS + 26 <= avail_h) {
        if (hit(x, y, bx, Y_UI_BTNS, 110, 26)) {
            i18n_set(LANG_EN); settings_save(); return;
        }
        if (hit(x, y, bx + 120, Y_UI_BTNS, 130, 26)) {
            i18n_set(LANG_RU); settings_save(); return;
        }
    }

    if (Y_TZ_BTNS + 26 <= avail_h) {
        int off = rtc_get_offset();
        int idx = tz_find_index(off);

        if (hit(x, y, bx, Y_TZ_BTNS, 32, 26)) {
            int new_idx;
            if (idx < 0) new_idx = 0;
            else         new_idx = (idx - 1 + TZ_COUNT) % TZ_COUNT;
            rtc_set_offset(TZ_LIST[new_idx].offset_min);
            settings_save();
            return;
        }
        if (hit(x, y, bx + 388, Y_TZ_BTNS, 32, 26)) {
            int new_idx;
            if (idx < 0) new_idx = 0;
            else         new_idx = (idx + 1) % TZ_COUNT;
            rtc_set_offset(TZ_LIST[new_idx].offset_min);
            settings_save();
            return;
        }
    }

    if (Y_WP_BTN + 26 <= avail_h) {
        if (hit(x, y, bx, Y_WP_BTN, 280, 26)) {
            extern void wallpaper_launch_settings(void);
            wallpaper_launch_settings();
            return;
        }
    }
    if (Y_CLOCK_BTNS + 26 <= avail_h) {
        if (hit(x, y, bx, Y_CLOCK_BTNS, 110, 26)) {
            g_clock_24h = 0; settings_save(); return;
        }
        if (hit(x, y, bx + 120, Y_CLOCK_BTNS, 110, 26)) {
            g_clock_24h = 1; settings_save(); return;
        }
    }
    if (Y_MOUSE_BTNS + 26 <= avail_h) {
        if (hit(x, y, bx, Y_MOUSE_BTNS, 100, 26)) {
            g_mouse_speed = 0; settings_save(); return;
        }
        if (hit(x, y, bx + 110, Y_MOUSE_BTNS, 100, 26)) {
            g_mouse_speed = 1; settings_save(); return;
        }
        if (hit(x, y, bx + 220, Y_MOUSE_BTNS, 100, 26)) {
            g_mouse_speed = 2; settings_save(); return;
        }
    }
}

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
    struct window *win = window_get(id);
    win->on_draw = set_draw;
    win->on_click = set_click;
    extern void window_reclamp(void);
    window_reclamp();
}