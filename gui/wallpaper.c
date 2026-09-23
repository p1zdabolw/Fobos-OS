#include "wallpaper.h"
#include "window.h"
#include "font.h"
#include "../kernel/fb.h"
#include "../kernel/timer.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../lib/i18n.h"

#define CACHE_MAX_W 1024
#define CACHE_MAX_H 768

extern void photos_render(int index, u32 *out, int w, int h);
extern int  photos_count(void);

static int g_mode = WALL_GRADIENT_V;
static int g_photo_index = 0;

static u32 g_photo_cache[CACHE_MAX_W * CACHE_MAX_H];
static int g_cache_w;
static int g_cache_h;
static int g_cache_valid;

static u32 hash32_w(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int clamp_u8_w(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static u32 lerp_color_w(u32 a, u32 b, int t, int max) {
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF;
    int ag = (a >> 8) & 0xFF;
    int ab = a & 0xFF;
    int br = (b >> 16) & 0xFF;
    int bg = (b >> 8) & 0xFF;
    int bb = b & 0xFF;
    int r = clamp_u8_w(ar + (br - ar) * t / max);
    int g = clamp_u8_w(ag + (bg - ag) * t / max);
    int bl = clamp_u8_w(ab + (bb - ab) * t / max);
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static u32 pattern_pixel(int mode, int x, int y, int w, int h) {
    switch (mode) {
        case WALL_SOLID:
            return 0x1B3A4B;

        case WALL_GRADIENT_V: {
            int t = y * 255 / (h > 0 ? h : 1);
            return lerp_color_w(0x0A1628, 0x3C6E9C, t, 255);
        }

        case WALL_GRADIENT_H: {
            int t = x * 255 / (w > 0 ? w : 1);
            return lerp_color_w(0x1B3A4B, 0xA86030, t, 255);
        }

        case WALL_CHECKER: {
            int cx = x / 32;
            int cy = y / 32;
            return ((cx + cy) & 1) ? 0x102028 : 0x1A2A3A;
        }

        case WALL_PLASMA: {
            int v1 = (x * 3 + y * 2) & 0xFF;
            int v2 = ((x * x + y * y) / 512) & 0xFF;
            int v3 = ((x - w / 2) * (y - h / 2) / 256) & 0xFF;
            int s = (v1 + v2 + v3) & 0xFF;
            int r = (s + 40) & 0xFF;
            int g = (s * 2 + 80) & 0xFF;
            int b = (s * 3 + 120) & 0xFF;
            return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }

        case WALL_STARFIELD: {
            u32 hv = hash32_w((u32)x / 4, (u32)y / 4);
            if ((hv & 0xFF) < 6) {
                u32 b = 0x80 + ((hv >> 8) & 0x7F);
                return (b << 16) | (b << 8) | b;
            }
            return 0x000005;
        }

        case WALL_DIAGONAL: {
            int d = (x + y) / 16;
            return (d & 1) ? 0x1B2A3C : 0x24384A;
        }

        case WALL_PHOTO:
            return 0x000000;
    }
    return 0x000000;
}

void wallpaper_init(void) {
    g_mode = WALL_GRADIENT_V;
    g_photo_index = 0;
    g_cache_valid = 0;
    g_cache_w = 0;
    g_cache_h = 0;
}

void wallpaper_set(int mode) {
    if (mode < 0 || mode >= WALL_COUNT) return;
    g_mode = mode;
    if (mode != WALL_PHOTO) g_cache_valid = 0;
}

void wallpaper_set_photo(int photo_index) {
    g_photo_index = photo_index;
    g_mode = WALL_PHOTO;
    g_cache_valid = 0;
}

int wallpaper_get(void) { return g_mode; }
int wallpaper_photo_index(void) { return g_photo_index; }

const char *wallpaper_name(int mode) {
    switch (mode) {
        case WALL_SOLID:      return tr(STR_WP_SOLID);
        case WALL_GRADIENT_V: return tr(STR_WP_GRAD_V);
        case WALL_GRADIENT_H: return tr(STR_WP_GRAD_H);
        case WALL_CHECKER:    return tr(STR_WP_CHECKER);
        case WALL_PLASMA:     return tr(STR_WP_PLASMA);
        case WALL_STARFIELD:  return tr(STR_WP_STARS);
        case WALL_DIAGONAL:   return tr(STR_WP_DIAGONAL);
        case WALL_PHOTO:      return tr(STR_WP_PHOTO);
        default:              return tr(STR_WP_TITLE);
    }
}

void wallpaper_draw(int x0, int y0, int w, int h) {
    if (g_mode == WALL_PHOTO) {
        if (!g_cache_valid || g_cache_w != w || g_cache_h != h) {
            if (w <= CACHE_MAX_W && h <= CACHE_MAX_H) {
                photos_render(g_photo_index, g_photo_cache, w, h);
                g_cache_w = w;
                g_cache_h = h;
                g_cache_valid = 1;
            } else {
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        fb_put_pixel(x0 + x, y0 + y, 0x101828);
                    }
                }
                return;
            }
        }
        for (int y = 0; y < h; y++) {
            u32 *src = g_photo_cache + (usize)y * (usize)w;
            for (int x = 0; x < w; x++) {
                fb_put_pixel(x0 + x, y0 + y, src[x]);
            }
        }
        return;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb_put_pixel(x0 + x, y0 + y, pattern_pixel(g_mode, x, y, w, h));
        }
    }
}

static int g_settings_window = -1;

#define SW_CELL_W 122
#define SW_CELL_H 128
#define SW_SWATCH_H 96
#define SW_COLS 4
#define SW_PAD 8

static void ws_draw_swatch(int x, int y, int w, int h, int mode) {
    if (mode == WALL_PHOTO) {
        int idx = g_photo_index;
        for (int py = 0; py < h; py++) {
            for (int px = 0; px < w; px++) {
                int sx = px * 32 / w;
                int sy = py * 32 / h;
                u32 hv = hash32_w((u32)sx, (u32)sy + (u32)idx * 47);
                u32 c = ((hv & 0xFF) << 16) | (((hv >> 8) & 0xFF) << 8) | ((hv >> 16) & 0xFF);
                fb_put_pixel(x + px, y + py, c);
            }
        }
        return;
    }
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            fb_put_pixel(x + px, y + py, pattern_pixel(mode, px, py, w, h));
        }
    }
}

static void ws_draw(struct window *win) {
    int bx = win->x + SW_PAD;
    int by = win->y + 22 + SW_PAD;
    int active = g_mode;

    for (int i = 0; i < WALL_COUNT; i++) {
        int col = i % SW_COLS;
        int row = i / SW_COLS;
        int cx = bx + col * SW_CELL_W;
        int cy = by + row * SW_CELL_H;

        int sel = (i == active);
        u32 border = sel ? 0x3C78B4 : 0x606870;
        fb_fill_rect(cx - 2, cy - 2, SW_CELL_W, SW_SWATCH_H + 24, border);
        fb_fill_rect(cx - 1, cy - 1, SW_CELL_W - 2, SW_SWATCH_H + 22, 0xE8E8E8);

        ws_draw_swatch(cx, cy, SW_CELL_W - 4, SW_SWATCH_H, i);

        const char *nm = wallpaper_name(i);
        int tw = font_text_width(nm);
        int tx = cx + (SW_CELL_W - 4 - tw) / 2;
        font_draw_string(tx, cy + SW_SWATCH_H + 2, nm, 0x101010, 0xE8E8E8);
    }

    int fy = by + 2 * SW_CELL_H + 8;
    char buf[80];
    snprintf(buf, sizeof(buf), tr(STR_WP_CURRENT), wallpaper_name(active));
    font_draw_string(win->x + SW_PAD, fy, buf, 0x101010, 0xE8E8E8);
}

static void ws_click(struct window *win, int x, int y) {
    (void)win;
    int bx = SW_PAD;
    int by = SW_PAD;

    for (int i = 0; i < WALL_COUNT; i++) {
        int col = i % SW_COLS;
        int row = i / SW_COLS;
        int cx = bx + col * SW_CELL_W;
        int cy = by + row * SW_CELL_H;
        if (x >= cx - 2 && x < cx + SW_CELL_W - 2 &&
            y >= cy - 2 && y < cy + SW_SWATCH_H + 22) {
            if (i == WALL_PHOTO) {
                int n = photos_count();
                if (n <= 0) n = 1;
                g_photo_index = (g_photo_index + 1) % n;
            }
            wallpaper_set(i);
            return;
        }
    }
}

void wallpaper_launch_settings(void) {
    for (int i = 0; i < window_count(); i++) {
        struct window *w = window_get(i);
        if (w && w->visible && w->on_click == ws_click) {
            window_focus(i);
            return;
        }
    }
    int w = SW_PAD * 2 + SW_COLS * SW_CELL_W;
    int h = 22 + SW_PAD * 2 + 2 * SW_CELL_H + 26;
    int id = window_create(150, 60, w, h, tr(STR_WP_TITLE));
    if (id < 0) return;
    g_settings_window = id;
    struct window *win = window_get(id);
    win->on_draw = ws_draw;
    win->on_click = ws_click;
}