/*
 * Fobos OS — реализация графики загрузчика (см. lgfx.h).
 * Шрифт — общий common/font8x8.h (включается через -Icommon).
 */
#include "lgfx.h"

#include "font8x8.h"
#include <fobos/boot_info.h>

#include <stddef.h>

#define LGFX_FIRST 0x20u

static struct {
    struct fbpi_framebuffer fb;
    u32 *base;
    u64 pitch_px;
    u32 w, h;
    int ready;
} g_lg;

int lgfx_init(const struct fbpi_framebuffer *fb)
{
    if (fb == NULL || fb->phys_addr == 0u ||
        fb->pitch_bytes < (u64)fb->width * 4u ||
        fb->width == 0u || fb->height == 0u) {
        return -1;
    }
    g_lg.fb = *fb;
    g_lg.base = (u32 *)(u64)fb->phys_addr;
    g_lg.pitch_px = fb->pitch_bytes / 4u;
    g_lg.w = fb->width;
    g_lg.h = fb->height;
    g_lg.ready = 1;
    return 0;
}

u32 lgfx_width(void)
{
    return g_lg.w;
}

u32 lgfx_height(void)
{
    return g_lg.h;
}

/* Нативный пиксель: общий конвертер fbpi (единый с ядром). */
static u32 lgfx_pixel(u32 rgb)
{
    return fbpi_pixel_rgb(&g_lg.fb, rgb);
}

void lgfx_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    if (g_lg.ready == 0) {
        return;
    }
    u32 px = lgfx_pixel(rgb);
    if (x >= g_lg.w || y >= g_lg.h) {
        return;
    }
    if (w > g_lg.w - x) {
        w = g_lg.w - x;
    }
    if (h > g_lg.h - y) {
        h = g_lg.h - y;
    }
    for (u32 yy = 0; yy < h; yy++) {
        u32 *row = g_lg.base + (u64)(y + yy) * g_lg.pitch_px + x;
        for (u32 xx = 0; xx < w; xx++) {
            row[xx] = px;
        }
    }
}

void lgfx_background(void)
{
    if (g_lg.ready == 0) {
        return;
    }
    for (u32 y = 0; y < g_lg.h; y++) {
        u32 t = (y * 255u) / (g_lg.h > 1u ? g_lg.h - 1u : 1u);
        u32 c = ((LGFX_COL_BG_TOP & 0xFFu) * (255u - t) + (LGFX_COL_BG_BOT & 0xFFu) * t) / 255u
              | ((((LGFX_COL_BG_TOP >> 8) & 0xFFu) * (255u - t) +
                  ((LGFX_COL_BG_BOT >> 8) & 0xFFu) * t) / 255u) << 8
              | ((((LGFX_COL_BG_TOP >> 16) & 0xFFu) * (255u - t) +
                  ((LGFX_COL_BG_BOT >> 16) & 0xFFu) * t) / 255u) << 16;
        lgfx_fill_rect(0, y, g_lg.w, 1u, c);
    }
}

void lgfx_glyph(u32 x, u32 y, const u8 rows[8], u32 fg, u32 bg)
{
    if (g_lg.ready == 0 || rows == NULL) {
        return;
    }
    u32 fpx = lgfx_pixel(fg);
    u32 bpx = lgfx_pixel(bg);
    for (u32 ry = 0; ry < 8u; ry++) {
        u8 bits = rows[ry];
        u32 *row = g_lg.base + (u64)(y + ry) * g_lg.pitch_px + x;
        for (u32 rx = 0; rx < 8u; rx++) {
            row[rx] = ((bits >> rx) & 1u) ? fpx : bpx;
        }
    }
}

void lgfx_text(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    if (g_lg.ready == 0 || s == NULL) {
        return;
    }
    for (u32 i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < (char)0x20 || c > (char)0x7Eu) {
            c = '.';
        }
        if (x + i * 8u + 8u > g_lg.w || y + 8u > g_lg.h) {
            break;
        }
        lgfx_glyph(x + i * 8u, y, font8x8[c - LGFX_FIRST], fg, bg);
    }
}

void lgfx_text_centered(u32 y, const char *s, u32 fg, u32 bg)
{
    if (s == NULL) {
        return;
    }
    u32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    u32 tw = n * 8u;
    u32 x = (g_lg.w > tw) ? (g_lg.w - tw) / 2u : 0u;
    lgfx_text(x, y, s, fg, bg);
}

void lgfx_box(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 border)
{
    lgfx_fill_rect(x, y, w, h, fill);
    lgfx_fill_rect(x, y, w, 1u, border);
    lgfx_fill_rect(x, y + (h > 0u ? h - 1u : 0u), w, 1u, border);
    lgfx_fill_rect(x, y, 1u, h, border);
    lgfx_fill_rect(x + (w > 0u ? w - 1u : 0u), y, 1u, h, border);
}

void lgfx_input_field(u32 x, u32 y, u32 w, const char *text,
                      u32 cursor_pos, int focused, int masked)
{
    u32 border = (focused != 0) ? LGFX_COL_BORDER_SEL : LGFX_COL_BORDER;
    lgfx_box(x, y, w, 16u, LGFX_COL_FIELD, border);

    u32 tx = x + 4u;
    u32 ty = y + 4u;
    if (text != NULL) {
        for (u32 i = 0; text[i] != '\0'; i++) {
            char c = (masked != 0) ? '*' : text[i];
            if (c < (char)0x20 || c > (char)0x7Eu) {
                c = '.';
            }
            if (tx + (i + 1u) * 8u > x + w - 4u) {
                break;
            }
            lgfx_glyph(tx + i * 8u, ty, font8x8[c - LGFX_FIRST],
                       LGFX_COL_TX, LGFX_COL_FIELD);
        }
    }
    if (focused != 0) {
        u32 cx = tx + cursor_pos * 8u;
        if (cx + 8u <= x + w - 3u) {
            lgfx_fill_rect(cx, ty, 8u, 8u, LGFX_COL_TX);
        }
    }
}
