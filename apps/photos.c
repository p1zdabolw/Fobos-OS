#include "photos.h"
#include "../gui/window.h"
#include "../gui/font.h"
#include "../gui/widget.h"
#include "../gui/wallpaper.h"
#include "../kernel/fb.h"
#include "../kernel/timer.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../lib/i18n.h"

enum {
    PHOTO_SKY,
    PHOTO_SUNSET,
    PHOTO_PLASMA,
    PHOTO_CHESS,
    PHOTO_MANDELBROT,
    PHOTO_STARS,
    PHOTO_RAINBOW,
    PHOTO_WAVES,
    PHOTO_COUNT
};

#define PHOTO_W 480
#define PHOTO_H 360
#define WIN_W   (PHOTO_W + 20)
#define WIN_H   (22 + 6 + PHOTO_H + 6 + 24 + 6)

static const int g_photo_str[PHOTO_COUNT] = {
    STR_PHOTO_SKY,
    STR_PHOTO_SUNSET,
    STR_PHOTO_PLASMA,
    STR_PHOTO_CHESS,
    STR_PHOTO_MANDELBROT,
    STR_PHOTO_STARS,
    STR_PHOTO_RAINBOW,
    STR_PHOTO_WAVES,
};

static u32 hash32_p(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int clamp_u8_p(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static u32 lerp_p(u32 a, u32 b, int t, int max) {
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF;
    int ag = (a >> 8) & 0xFF;
    int ab = a & 0xFF;
    int br = (b >> 16) & 0xFF;
    int bg = (b >> 8) & 0xFF;
    int bb = b & 0xFF;
    int r = clamp_u8_p(ar + (br - ar) * t / max);
    int g = clamp_u8_p(ag + (bg - ag) * t / max);
    int bl = clamp_u8_p(ab + (bb - ab) * t / max);
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static u32 hsv_p(int h, int s, int v) {
    h = ((h % 360) + 360) % 360;
    int c = v * s / 255;
    int hp = h / 60;
    int f = h % 60;
    int x;
    if (hp % 2 == 0) x = c * f / 60;
    else             x = c * (60 - f) / 60;
    int m = v - c;
    int r, g, b;
    switch (hp) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }
    return ((u32)(r + m) << 16) | ((u32)(g + m) << 8) | (u32)(b + m);
}

static void render_sky(u32 *out, int w, int h) {
    for (int y = 0; y < h; y++) {
        int t = y * 255 / h;
        u32 bg = lerp_p(0x1A5BA8, 0x88C8F0, t, 255);
        for (int x = 0; x < w; x++) {
            u32 c = bg;
            if (y > h / 5 && y < h * 3 / 4) {
                u32 hv = hash32_p((u32)x / 24, (u32)y / 12);
                if ((hv & 0xFF) < 50) {
                    int a = (int)((hv >> 8) & 0x5F);
                    c = lerp_p(bg, 0xFFFFFF, a, 0x5F);
                }
            }
            out[y * w + x] = c;
        }
    }
}

static void render_sunset(u32 *out, int w, int h) {
    for (int y = 0; y < h; y++) {
        int t = y * 255 / h;
        u32 c;
        if (t < 128) c = lerp_p(0x201838, 0xE06030, t, 128);
        else         c = lerp_p(0xE06030, 0xFFE090, t - 128, 127);
        for (int x = 0; x < w; x++) {
            out[y * w + x] = c;
        }
    }
    int cx = w / 2;
    int cy = h * 2 / 3;
    int r = h / 8;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                int px = cx + dx;
                int py = cy + dy;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    out[py * w + px] = 0xFFF6C8;
                }
            }
        }
    }
    for (int x = 0; x < w; x++) {
        int base = h - 1;
        for (int y = 0; y < h / 8; y++) {
            int py = base - y;
            u32 shade = (u32)(0x101828 + ((y * 3) << 8)) & 0xFFFFFFu;
            if (py >= 0) out[py * w + x] = shade;
        }
    }
}

static void render_plasma(u32 *out, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v1 = (x * 3 + y * 2) & 0xFF;
            int v2 = ((x * x + y * y) / 128) & 0xFF;
            int v3 = ((x - w / 2) * (y - h / 2) / 64) & 0xFF;
            int s = (v1 + v2 + v3) & 0xFF;
            int r = (s + 60) & 0xFF;
            int g = (s * 2 + 80) & 0xFF;
            int b = (s * 3 + 120) & 0xFF;
            out[y * w + x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
}

static void render_chess(u32 *out, int w, int h) {
    int cell = h / 8;
    if (cell < 4) cell = 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int cx = x / cell;
            int cy = y / cell;
            out[y * w + x] = ((cx + cy) & 1) ? 0xF0F0F0 : 0x101820;
        }
    }
}

static void render_mandelbrot(u32 *out, int w, int h) {
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int cr = px * 400 / w - 300;
            int ci = py * 400 / h - 200;
            int zr = 0;
            int zi = 0;
            int iter = 0;
            int max_iter = 24;
            while (iter < max_iter) {
                int zr2 = zr * zr / 100;
                int zi2 = zi * zi / 100;
                if (zr2 + zi2 > 400) break;
                int nzr = zr2 - zi2 + cr;
                int nzi = 2 * zr * zi / 100 + ci;
                zr = nzr;
                zi = nzi;
                iter++;
            }
            if (iter >= max_iter) {
                out[py * w + px] = 0x000000;
            } else {
                int hue = (iter * 15) % 360;
                out[py * w + px] = hsv_p(hue, 220, 200);
            }
        }
    }
}

static void render_stars(u32 *out, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 hv = hash32_p((u32)x, (u32)y);
            if ((hv & 0x3FF) < 3) {
                int b = 0x80 + (int)((hv >> 10) & 0x7F);
                out[y * w + x] = ((u32)b << 16) | ((u32)b << 8) | (u32)b;
            } else {
                out[y * w + x] = 0x000010;
            }
        }
    }
    for (int i = 0; i < 24; i++) {
        u32 hv = hash32_p((u32)i * 17, 0x5A5Au);
        int cx = (int)(hv % (u32)w);
        int cy = (int)((hv >> 16) % (u32)h);
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int d = dx * dx + dy * dy;
                int px = cx + dx;
                int py = cy + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                if (d == 0)      out[py * w + px] = 0xFFFFFF;
                else if (d <= 2) out[py * w + px] = 0xC0D0E8;
                else if (d <= 5) out[py * w + px] = 0x708090;
            }
        }
    }
}

static void render_rainbow(u32 *out, int w, int h) {
    int cx = w / 2;
    int cy = h / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            int ang = (d2 / 300) % 360;
            out[y * w + x] = hsv_p(ang, 240, 220);
        }
    }
}

static void render_waves(u32 *out, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v = 0;
            v += (x * 8) & 0xFF;
            v += (y * 5) & 0xFF;
            v += (x * y / 24) & 0xFF;
            int s = v & 0xFF;
            int r = (s + 60) & 0xFF;
            int g = (s * 5 / 3 + 80) & 0xFF;
            int b = (s * 7 / 4 + 40) & 0xFF;
            out[y * w + x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
}

void photos_render(int index, u32 *out, int w, int h) {
    switch (index) {
        case PHOTO_SKY:        render_sky(out, w, h); break;
        case PHOTO_SUNSET:     render_sunset(out, w, h); break;
        case PHOTO_PLASMA:     render_plasma(out, w, h); break;
        case PHOTO_CHESS:      render_chess(out, w, h); break;
        case PHOTO_MANDELBROT: render_mandelbrot(out, w, h); break;
        case PHOTO_STARS:      render_stars(out, w, h); break;
        case PHOTO_RAINBOW:    render_rainbow(out, w, h); break;
        case PHOTO_WAVES:      render_waves(out, w, h); break;
        default:               render_sky(out, w, h); break;
    }
}

int photos_count(void) { return PHOTO_COUNT; }

struct photo_state {
    int  index;
    int  dirty;
    int  slideshow;
    u64  last_slide;
    u32  buffer[PHOTO_W * PHOTO_H];
};

static struct photo_state g_photos[MAX_WINDOWS];

static void photo_buttons(int win_x, int win_y, struct widget_rect *prev,
                          struct widget_rect *next, struct widget_rect *setw,
                          struct widget_rect *slide) {
    int fy = win_y + 22 + 6 + PHOTO_H + 6;
    prev->x = win_x + 6;   prev->y = fy; prev->w = 70; prev->h = 22;
    next->x = win_x + 80;  next->y = fy; next->w = 70; next->h = 22;
    setw->x = win_x + 154; setw->y = fy; setw->w = 130; setw->h = 22;
    slide->x = win_x + 288; slide->y = fy; slide->w = 100; slide->h = 22;
}

static void photo_draw(struct window *win) {
    struct photo_state *st = (struct photo_state*)win->user;

    if (st->slideshow && timer_ticks() - st->last_slide > 400) {
        st->index = (st->index + 1) % PHOTO_COUNT;
        st->dirty = 1;
        st->last_slide = timer_ticks();
    }
    if (st->dirty) {
        photos_render(st->index, st->buffer, PHOTO_W, PHOTO_H);
        st->dirty = 0;
    }

    int px = win->x + 10;
    int py = win->y + 22 + 6;
    for (int y = 0; y < PHOTO_H; y++) {
        u32 *src = st->buffer + (usize)y * PHOTO_W;
        for (int x = 0; x < PHOTO_W; x++) {
            fb_put_pixel(px + x, py + y, src[x]);
        }
    }

    int fy = py + PHOTO_H + 4;
    char buf[80];
    snprintf(buf, sizeof(buf), "%s  (%d/%d)",
             tr(g_photo_str[st->index]), st->index + 1, PHOTO_COUNT);
    font_draw_string(win->x + 10, fy, buf, 0x101010, 0xE8E8E8);

    struct widget_rect bprev, bnext, bset, bslide;
    photo_buttons(win->x, win->y, &bprev, &bnext, &bset, &bslide);
    widget_draw_button(&bprev, tr(STR_PHOTOS_PREV), 0);
    widget_draw_button(&bnext, tr(STR_PHOTOS_NEXT), 0);
    widget_draw_button(&bset, tr(STR_PHOTOS_SET), 0);
    widget_draw_button(&bslide, st->slideshow ? tr(STR_PHOTOS_STOP) : tr(STR_PHOTOS_SLIDE), 0);
}

static void photo_click(struct window *win, int x, int y) {
    struct photo_state *st = (struct photo_state*)win->user;
    struct widget_rect bprev, bnext, bset, bslide;
    photo_buttons(0, 0, &bprev, &bnext, &bset, &bslide);

    if (widget_point_in(&bprev, x, y)) {
        st->index = (st->index + PHOTO_COUNT - 1) % PHOTO_COUNT;
        st->dirty = 1;
        return;
    }
    if (widget_point_in(&bnext, x, y)) {
        st->index = (st->index + 1) % PHOTO_COUNT;
        st->dirty = 1;
        return;
    }
    if (widget_point_in(&bset, x, y)) {
        wallpaper_set_photo(st->index);
        return;
    }
    if (widget_point_in(&bslide, x, y)) {
        st->slideshow = !st->slideshow;
        st->last_slide = timer_ticks();
        return;
    }
}

static struct photo_state *photo_alloc(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_photos[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) if (!used[i]) return &g_photos[i];
    return NULL;
}

void apps_launch_photos(void) {
    struct photo_state *st = photo_alloc();
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->index = 0;
    st->dirty = 1;
    st->slideshow = 0;
    st->last_slide = timer_ticks();

    int id = window_create(140, 90, WIN_W, WIN_H, tr(STR_APP_PHOTOS));
    if (id < 0) return;
    struct window *win = window_get(id);
    win->user = st;
    win->on_draw = photo_draw;
    win->on_click = photo_click;
}