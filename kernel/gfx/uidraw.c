/*
 * Fobos OS — реализация uidraw (см. uidraw.h).
 */
#include "uidraw.h"

#include "font8x8.h"
#include "../drivers/fb.h"
#include "../drivers/mouse.h"
#include "../lib/string.h"

#define CUR_W 12u
#define CUR_H 18u

static struct {
    u32 w, h;
    int ready;
    u32 mouse_x, mouse_y;
    u32 saved_bg[CUR_W * CUR_H];
    /* FND-2f: кадровый протокол курсора. deferred=1 (TUI): события мыши
     * меняют только координаты — спрайт рисует кадровый конвейер
     * (main_loop), зона старого спрайта перекрашивается из ячеек консоли.
     * stamp_* — где СПРАЙТ сейчас напечатан (для точечной перекраски). */
    int deferred;
    int stamp_valid;
    u32 stamp_x, stamp_y;
} g_ud;

/* Тема (FND-2e): акцент и вариант обоев. Инициализированы по умолчанию;
 * настраиваются через uidraw_set_accent/uidraw_set_wall (Settings). */
static u32 g_accent = UI_COL_ACCENT;
static u32 g_wall   = UI_WALL_BLOOM;

u32 uidraw_accent(void)
{
    return g_accent;
}

void uidraw_set_accent(u32 rgb)
{
    g_accent = rgb & 0xFFFFFFu;
    /* Акцент участвует в градиентных обоях? Нет, но логотип/кэш
     * консистентнее сбрасывать — дешёвая операция. */
    fb_walllayer_invalidate();
}

void uidraw_set_wall(u32 id)
{
    u32 next = (id < UI_WALL_COUNT) ? id : UI_WALL_GRADIENT;
    if (next != g_wall) {
        g_wall = next;
        fb_walllayer_invalidate();
    }
}

u32 uidraw_wall(void)
{
    return g_wall;
}

/* Классическая стрелка: 0 — прозрачно, 1 — контур, 2 — заливка. */
static const u8 cursor_sprite[CUR_H][CUR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,2,2,1,0},
    {0,0,0,0,0,0,0,0,1,1,1,0},
};

err_t uidraw_init(void)
{
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    g_ud.w = w;
    g_ud.h = h;
    g_ud.mouse_x = w / 2u;
    g_ud.mouse_y = h / 2u;
    g_ud.ready = 1;
    return K_OK;
}

u32 uidraw_width(void)
{
    return g_ud.w;
}

u32 uidraw_height(void)
{
    return g_ud.h;
}

/* Целочисленный квадратный корень (bit-by-bit, без библиотек). */
static u32 ud_isqrt(u64 v)
{
    u64 res = 0;
    u64 bit = 1ull << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (u32)res;
}

/* Залитый эллипс (центр cx,cy; полуоси rx,ry) — построчными спанами. */
static void ud_fill_ellipse(s32 cx, s32 cy, u32 rx, u32 ry, u32 col)
{
    if (rx == 0u || ry == 0u) {
        return;
    }
    for (s32 dy = -(s32)ry; dy <= (s32)ry; dy++) {
        /* dx = rx * sqrt(1 - (dy/ry)^2) */
        u64 t = (u64)(dy < 0 ? -dy : dy);
        t = (t * t) * ((u64)rx * (u64)rx);
        u64 k  = (u64)ry * (u64)ry;
        if (t > k) {
            continue;
        }
        u32 dx = (u32)((u64)rx * ud_isqrt(k - t) / (u64)ry);
        (void)fb_fill_rect((u32)(cx - (s32)dx), (u32)(cy + dy),
                           dx * 2u + 1u, 1u, col);
    }
}

/* Линейная интерполяция цвета 0x00RRGGBB: t = 0..255. */
static u32 ud_mix(u32 a, u32 b, u32 t)
{
    u32 ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    u32 br = (b >> 16) & 0xFFu, bg2 = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    u32 r = ar + ((br - ar) * t) / 255u;
    u32 g = ag + ((bg2 - ag) * t) / 255u;
    u32 bl = ab + ((bb - ab) * t) / 255u;
    return (r << 16) | (g << 8) | bl;
}

/* Фоновый вертикальный градиент между двумя цветами. */
static void ud_bg_gradient(u32 top, u32 bot)
{
    for (u32 y = 0; y < g_ud.h; y++) {
        u32 t = (y * 255u) / (g_ud.h > 1u ? g_ud.h - 1u : 1u);
        (void)fb_fill_rect(0, y, g_ud.w, 1u, ud_mix(top, bot, t));
    }
}

/* WALL_BLOOM: концентрические «ленты»-эллипсы в сине-фиолетовой гамме —
 * процедурный аналог обоев-«цветка» Win11. Построчные спаны: быстро. */
static void ud_wall_bloom(void)
{
    ud_bg_gradient(0x101828u, 0x05060Au);

    static const u32 stops[5] = {
        0x123B66u, 0x1C5FA8u, 0x2E7BD0u, 0x4B6FD6u, 0x7B5CD6u
    };
    s32 cx = (s32)(g_ud.w * 62u / 100u);
    s32 cy = (s32)(g_ud.h * 40u / 100u);
    u32 rx_max = g_ud.w * 42u / 100u;

    /* 12 лент: от широкой тёмной к яркой сердцевине. */
    for (u32 i = 0; i < 12u; i++) {
        /* t=0 внешний край, t=255 центр. */
        u32 t = (i * 255u) / 11u;
        u32 col;
        if (t < 128u) {
            col = ud_mix(stops[0], stops[1], t * 2u);
        } else if (t < 192u) {
            col = ud_mix(stops[2], stops[3], (t - 128u) * 4u);
        } else {
            col = ud_mix(stops[3], stops[4], (t - 192u) * 4u);
        }
        /* Радиусы — по квадрату остатка: плотнее к центру, без
         * арифметических переполнений (i = 0..11 → множитель 144..1/144). */
        u32 rem = 12u - i;   /* 12..1 */
        u32 rx = rx_max * rem * rem / 144u;
        if (rx < 6u) {
            rx = 6u;
        }
        u32 ry = rx * 58u / 100u;
        ud_fill_ellipse(cx, cy, rx, ry, col);
    }
    /* Сердцевина. */
    ud_fill_ellipse(cx, cy, rx_max * 10u / 100u + 4u,
                    (rx_max * 10u / 100u + 4u) * 58u / 100u, 0x8FB8E8u);
}

/* WALL_HILLS: тёмное небо, солнце, два зелёных холма. */
static void ud_wall_hills(void)
{
    ud_bg_gradient(0x0D1626u, 0x1A2A44u);

    s32 cx = (s32)(g_ud.w * 24u / 100u);
    s32 cy = (s32)(g_ud.h * 22u / 100u);
    ud_fill_ellipse(cx, cy, g_ud.h / 14u, g_ud.h / 14u, 0xE8D28Au);

    ud_fill_ellipse((s32)(g_ud.w * 30u / 100u), (s32)(g_ud.h * 130u / 100u),
                    g_ud.w * 60u / 100u, g_ud.h * 75u / 100u, 0x2E5B3Au);
    ud_fill_ellipse((s32)(g_ud.w * 85u / 100u), (s32)(g_ud.h * 145u / 100u),
                    g_ud.w * 70u / 100u, g_ud.h * 90u / 100u, 0x1F4530u);
}

/* WALL_AURORA: вертикальные синус-ленты. */
static void ud_wall_aurora(void)
{
    ud_bg_gradient(0x07101Cu, 0x04070Cu);

    static const u32 cols[3] = { 0x1F7A5Cu, 0x2E8BC0u, 0x5C4F9Du };
    static const s32 amp[3] = { 60, 40, 52 };
    static const u32 hgt[3] = { 90, 64, 78 };

    for (u32 band = 0; band < 3u; band++) {
        for (u32 x = 0; x + 3u < g_ud.w; x += 3u) {
            /* Полный период синуса на два экрана: phase = 0..15. */
            static const s32 sine[17] = {
                0, 38, 71, 92, 100, 92, 71, 38, 0,
                -38, -71, -92, -100, -92, -71, -38, 0
            };
            u32 phase = (x * 32u / (g_ud.w > 0u ? g_ud.w : 1u)) % 16u;
            s32 y0 = (s32)(g_ud.h * 38u / 100u)
                     + sine[phase] * amp[band] / 100u
                     + (s32)(band * g_ud.h / 12u);
            u32 col = ud_mix(cols[band], 0x04070Cu,
                             140u + (x % 24u) * 4u);
            (void)fb_fill_rect(x, (u32)(y0 < 0 ? 0 : y0), 3u, hgt[band], col);
        }
    }
}

void uidraw_background(void)
{
    if (g_ud.ready == 0) {
        return;
    }
    /* Кэш обоев: процедурный фон дорог (тысячи fill_rect под TCG).
     * Он рисуется один раз в отдельный слой; каждый кадр начинается
     * с быстрого row-copy слоя в back buffer. */
    if (fb_walllayer_active() != 0) {
        if (fb_walllayer_dirty() != 0) {
            fb_surface_push(fb_walllayer_base());
            fb_clip_screen();
            switch (g_wall) {
            case UI_WALL_BLOOM:  ud_wall_bloom();  break;
            case UI_WALL_HILLS:  ud_wall_hills();  break;
            case UI_WALL_AURORA: ud_wall_aurora(); break;
            default:             ud_bg_gradient(UI_COL_BG_TOP, UI_COL_BG_BOT); break;
            }
            fb_surface_pop();
            fb_walllayer_mark_clean();
        }
        fb_walllayer_restore();
        return;
    }

    switch (g_wall) {
    case UI_WALL_BLOOM:
        ud_wall_bloom();
        break;
    case UI_WALL_HILLS:
        ud_wall_hills();
        break;
    case UI_WALL_AURORA:
        ud_wall_aurora();
        break;
    default:
        ud_bg_gradient(UI_COL_BG_TOP, UI_COL_BG_BOT);
        break;
    }
}

static u32 text_width(const char *s)
{
    return (u32)strlen(s) * UI_TEXT_W;
}

void uidraw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    if (g_ud.ready == 0 || s == NULL) {
        return;
    }
    for (u32 i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < (char)0x20 || c > (char)0x7Eu) {
            c = '.';
        }
        (void)fb_blit_glyph8(x + i * UI_TEXT_W, y,
                             font8x8[(u8)c - FONT8X8_FIRST], fg, bg);
    }
}

u32 uidraw_text_fit(u32 x, u32 y, u32 max_w, const char *s, u32 fg, u32 bg)
{
    if (g_ud.ready == 0 || s == NULL) {
        return 0;
    }
    u32 max_chars = max_w / UI_TEXT_W;
    u32 n = (u32)strlen(s);

    if (n <= max_chars) {
        uidraw_text(x, y, s, fg, bg);
        return n * UI_TEXT_W;
    }
    if (max_chars < 2u) {
        return 0;   /* место нет даже на ".." — честно не рисуем ничего */
    }
    /* Рисуем (max_chars - 2) символа + "..": усечение видно пользователю. */
    u32 keep = max_chars - 2u;
    for (u32 i = 0; i < keep; i++) {
        char c = s[i];
        if (c < (char)0x20 || c > (char)0x7Eu) {
            c = '.';
        }
        (void)fb_blit_glyph8(x + i * UI_TEXT_W, y,
                             font8x8[(u8)c - FONT8X8_FIRST], fg, bg);
    }
    (void)fb_blit_glyph8(x + keep * UI_TEXT_W, y,
                         font8x8['.' - FONT8X8_FIRST], fg, bg);
    (void)fb_blit_glyph8(x + (keep + 1u) * UI_TEXT_W, y,
                         font8x8['.' - FONT8X8_FIRST], fg, bg);
    return max_chars * UI_TEXT_W;
}

u32 uidraw_text_wrap(u32 x, u32 y, u32 max_w, u32 max_lines, u32 line_h,
                     const char *s, u32 fg, u32 bg)
{
    if (g_ud.ready == 0 || s == NULL || max_lines == 0u) {
        return 0;
    }
    u32 max_chars = max_w / UI_TEXT_W;
    if (max_chars == 0u) {
        return 0;
    }

    u32 lines = 0;
    u32 i = 0;
    while (s[i] != '\0' && lines < max_lines) {
        /* Пропускаем ведущие пробелы. */
        while (s[i] == ' ') {
            i++;
        }
        /* Собираем строку до лимита символов, разрыв — по последнему
         * пробелу, если он есть дальше начала строки. */
        u32 start = i;
        u32 len = 0;
        u32 last_space = 0;
        int have_space = 0;
        while (s[i] != '\0' && s[i] != '\n' && len < max_chars) {
            if (s[i] == ' ') {
                last_space = len;
                have_space = 1;
            }
            i++;
            len++;
        }
        u32 draw_len = len;
        if (s[i] != '\0' && s[i] != '\n' && have_space != 0 &&
            last_space > 0u) {
            /* Строка оборвалась на слове — переносим по пробелу. */
            draw_len = last_space;
            i = start + last_space + 1u;
        }
        while (s[i] == ' ') {
            i++;   /* перенос съедает пробелы после разрыва */
        }
        if (draw_len == 0u) {
            draw_len = 1u;   /* защита от зависания на длинных словах */
        }

        /* Печать строки [start, start+draw_len). */
        for (u32 k = 0; k < draw_len; k++) {
            char c = s[start + k];
            if (c < (char)0x20 || c > (char)0x7Eu) {
                c = '.';
            }
            (void)fb_blit_glyph8(x + k * UI_TEXT_W, y + lines * line_h,
                                 font8x8[(u8)c - FONT8X8_FIRST], fg, bg);
        }
        lines++;
        if (s[i] == '\n') {
            i++;
        }
    }
    return lines;
}

void uidraw_text_centered(u32 y, const char *s, u32 fg, u32 bg)
{
    if (g_ud.ready == 0 || s == NULL) {
        return;
    }
    u32 tw = text_width(s);
    u32 x = (g_ud.w > tw) ? (g_ud.w - tw) / 2u : 0u;
    uidraw_text(x, y, s, fg, bg);
}

void uidraw_box(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 border)
{
    (void)fb_fill_rect(x, y, w, h, fill);
    (void)fb_fill_rect(x, y, w, 1u, border);
    (void)fb_fill_rect(x, y + (h > 0u ? h - 1u : 0u), w, 1u, border);
    (void)fb_fill_rect(x, y, 1u, h, border);
    (void)fb_fill_rect(x + (w > 0u ? w - 1u : 0u), y, 1u, h, border);
}

void uidraw_input_field(u32 x, u32 y, u32 w, const char *text,
                        u32 cursor_pos, int focused, int masked)
{
    u32 border = (focused != 0) ? UI_COL_BORDER_SEL : UI_COL_BORDER;
    uidraw_box(x, y, w, UI_TEXT_H + 8u, UI_COL_FIELD, border);

    u32 tx = x + 4u;
    u32 ty = y + 4u;
    if (text != NULL) {
        for (u32 i = 0; text[i] != '\0'; i++) {
            char c = (masked != 0) ? '*' : text[i];
            if (c < (char)0x20 || c > (char)0x7Eu) {
                c = '.';
            }
            if (tx + (i + 1u) * UI_TEXT_W > x + w - 4u) {
                break;
            }
            (void)fb_blit_glyph8(tx + i * UI_TEXT_W, ty,
                                 font8x8[c - FONT8X8_FIRST], UI_COL_TX, UI_COL_FIELD);
        }
    }

    /* Блок-курсор (мигание не требуется на FND-2b — статичный курсор). */
    if (focused != 0) {
        u32 cx = tx + cursor_pos * UI_TEXT_W;
        if (cx + UI_TEXT_W <= x + w - 3u) {
            (void)fb_fill_rect(cx, ty, UI_TEXT_W, UI_TEXT_H, UI_COL_TX);
        }
    }
}

void uidraw_button(u32 x, u32 y, u32 w, const char *label, int focused)
{
    u32 fill = (focused != 0) ? UI_COL_ACCENT : UI_COL_PANEL;
    u32 tx   = (focused != 0) ? 0x0B0D11u : UI_COL_TX;
    u32 bd   = (focused != 0) ? UI_COL_ACCENT : UI_COL_BORDER;
    (void)fb_fill_rect(x, y, w, UI_BTN_H, fill);
    (void)fb_fill_rect(x, y, w, 1u, bd);
    (void)fb_fill_rect(x, y + UI_BTN_H - 1u, w, 1u, bd);
    (void)fb_fill_rect(x, y, 1u, UI_BTN_H, bd);
    (void)fb_fill_rect(x + w - 1u, y, 1u, UI_BTN_H, bd);

    if (label != NULL) {
        u32 tw = text_width(label);
        u32 lx = (w > tw) ? x + (w - tw) / 2u : x;
        u32 ly = y + (UI_BTN_H - UI_TEXT_H) / 2u;
        uidraw_text(lx, ly, label, tx, fill);
    }
}

int uidraw_button_hit(u32 x, u32 y, u32 w, u32 mx, u32 my)
{
    return uidraw_rect_hit(x, y, w, UI_BTN_H, mx, my);
}

int uidraw_rect_hit(u32 x, u32 y, u32 w, u32 h, u32 mx, u32 my)
{
    return (mx >= x && mx < x + w && my >= y && my < y + h) ? 1 : 0;
}

/* --- Процедурные иконки приложений (см. uidraw.h) --- */

/* Заливка с зажимом: локальные прямоугольники относительно (x, y). */
static void ud_r(u32 x, u32 y, u32 rx, u32 ry, u32 rw, u32 rh, u32 col)
{
    (void)fb_fill_rect(x + rx, y + ry, rw, rh, col);
}

/* Скруглённая плитка (углы 2px): заливка + рамка. База всех иконок-окон. */
static void ud_tile(u32 x, u32 y, u32 s, u32 fill, u32 border)
{
    (void)fb_fill_rect(x + 1u, y, s - 2u, s, fill);
    (void)fb_fill_rect(x, y + 1u, s, s - 2u, fill);
    (void)fb_fill_rect(x + 1u, y, s - 2u, 1u, border);
    (void)fb_fill_rect(x + 1u, y + s - 1u, s - 2u, 1u, border);
    (void)fb_fill_rect(x, y + 1u, 1u, s - 2u, border);
    (void)fb_fill_rect(x + s - 1u, y + 1u, 1u, s - 2u, border);
}

/* Диагональный штрих ">" (шеврон): две ступени толщиной th. */
static void ud_chevron(u32 x, u32 y, u32 leg, u32 th, u32 col)
{
    for (u32 i = 0; i < leg; i++) {
        ud_r(x + i * th, y + i * th, 0u, 0u, th, th, col);
        ud_r(x + i * th, y + (2u * leg - 1u - i) * th, 0u, 0u, th, th, col);
    }
}

void uidraw_app_icon(u32 kind, u32 x, u32 y, u32 size, int focused)
{
    if (size < 12u) {
        return;
    }
    u32 s = size;

    if (focused != 0) {
        /* Подложка-подсветка (hover/выделение) — скруглённая имитация:
         * прямоугольник + вырезы по углам. */
        (void)fb_fill_rect(x, y + 2u, s, s - 4u, 0x2A3646u);
        (void)fb_fill_rect(x + 2u, y, s - 4u, s, 0x2A3646u);
    }

    switch (kind) {
    case UI_ICON_CMD: {
        /* Окно терминала: тёмная плитка, заголовок с кнопками, шеврон. */
        ud_tile(x, y, s, 0x0B1017u, 0x33404Eu);
        u32 th = s * 16u / 100u;
        if (th < 2u) { th = 2u; }
        ud_r(x, y + 2u, 1u, 1u, s - 4u, th, 0x1C2836u);
        /* Три кнопки окна (классика). */
        u32 dy = y + 2u + th / 2u;
        u32 dd = (th >= 3u) ? 2u : 1u;
        ud_r(x + s * 14u / 100u, dy, 0u, 0u, dd, dd, 0xE5484Du);
        ud_r(x + s * 14u / 100u + dd + 1u, dy, 0u, 0u, dd, dd, 0xF0D000u);
        ud_r(x + s * 14u / 100u + 2u * (dd + 1u), dy, 0u, 0u, dd, dd,
             0x3FB950u);
        /* Приглашение ">_": шеврон + подчёркивание. */
        u32 leg = (s >= 26u) ? 3u : 2u;
        u32 cw = 2u;
        ud_chevron(x + s * 24u / 100u, y + s * 34u / 100u, leg, cw, 0x5CD6F5u);
        u32 uy = y + s * 34u / 100u + 2u * leg * cw + cw;
        if (uy + cw < y + s - 3u) {
            ud_r(x + s * 24u / 100u + 2u * leg * cw + cw, uy, 0u, 0u,
                 s * 22u / 100u, cw, 0x5CD6F5u);
        }
        break;
    }
    case UI_ICON_EXPLORER: {
        /* Папка Win11: язычок, задняя стенка, передняя створка светлее. */
        u32 bx = x + s * 6u / 100u;
        u32 by = y + s * 24u / 100u;
        u32 bw = s * 88u / 100u;
        u32 bh = s * 58u / 100u;
        /* Язычок. */
        ud_r(x, y, s * 8u / 100u, s * 14u / 100u, s * 36u / 100u,
             s * 12u / 100u, 0xC8871Cu);
        ud_r(x, y, s * 8u / 100u, s * 14u / 100u, s * 36u / 100u, 1u,
             0xE8A62Eu);
        /* Задняя стенка. */
        ud_r(bx, by, 0u, 0u, bw, bh, 0xD89A28u);
        /* Передняя створка (светлее, с бликом сверху). */
        u32 fy = by + bh * 22u / 100u;
        ud_r(bx, fy, 0u, 0u, bw, by + bh - fy, 0xF7C948u);
        ud_r(bx, fy, 0u, 0u, bw, (s >= 22u) ? 2u : 1u, 0xFFE59Eu);
        /* Нижняя тень. */
        ud_r(bx, by + bh - 1u, 0u, 0u, bw, 1u, 0xA87818u);
        break;
    }
    case UI_ICON_ABOUT: {
        /* Диск с "i": внешнее кольцо светлее — глубина. */
        u32 cx = x + s / 2u;
        u32 cy = y + s / 2u;
        u32 r = s * 42u / 100u;
        ud_fill_ellipse((s32)cx, (s32)cy, r, r, 0x1E5FA0u);
        ud_fill_ellipse((s32)cx, (s32)cy, r * 82u / 100u, r * 82u / 100u,
                        0x3E8BE0u);
        ud_fill_ellipse((s32)cx - r * 22u / 100u, (s32)cy - r * 25u / 100u,
                        r * 34u / 100u, r * 22u / 100u, 0x6FA8F5u);
        u32 iw = (s >= 26u) ? 3u : 2u;
        u32 ix = s / 2u - iw / 2u;
        ud_r(cx, y, ix, s * 30u / 100u, iw, iw, 0xF2F4F8u);
        ud_r(cx, y, ix, s * 44u / 100u, iw, s * 28u / 100u, 0xF2F4F8u);
        break;
    }
    case UI_ICON_VIEWER:
    case UI_ICON_NOTEPAD: {
        /* Страница с загнутым углом и строками. */
        u32 px = x + s * 16u / 100u;
        u32 pw = s * 62u / 100u;
        u32 py = y + s * 8u / 100u;
        u32 ph = s * 84u / 100u;
        u32 fold = s * 14u / 100u;
        /* Тело страницы (правый верхний угол под сгиб). */
        ud_r(px, py, 0u, 0u, pw, ph, 0xE8ECF2u);
        ud_r(px, py, 0u, 0u, pw - fold, ph, 0xF7F9FCu);
        /* Сгиб. */
        ud_r(px + pw - fold, py, 0u, 0u, fold, fold, 0xC3CCD8u);
        ud_r(px + pw - fold, py + fold - 1u, 0u, 0u, fold, 1u, 0xAAB6C4u);
        /* Заголовок и строки. */
        if (kind == UI_ICON_NOTEPAD) {
            ud_r(px + 2u, py + 2u, 0u, 0u, pw - 4u, s * 14u / 100u,
                 0x3E8BE0u);
        }
        u32 ly = py + (kind == UI_ICON_NOTEPAD ? s * 26u / 100u
                                               : s * 16u / 100u);
        for (u32 i = 0; i < 4u && ly + 3u < py + ph - 2u; i++) {
            u32 lw = pw - 8u - (i == 3u ? pw * 28u / 100u : 0u);
            ud_r(px + 4u, ly + i * (s * 13u / 100u), 0u, 0u,
                 (lw > fold) ? lw : 2u, 2u, 0x9AA7B4u);
        }
        break;
    }
    case UI_ICON_CALC: {
        /* Корпус, дисплей, сетка клавиш с акцентной колонкой. */
        ud_tile(x, y, s, 0x141B24u, 0x33404Eu);
        ud_r(x, y, s * 14u / 100u, s * 12u / 100u, s * 72u / 100u,
             s * 18u / 100u, 0x0E1116u);
        ud_r(x, y, s * 16u / 100u, s * 14u / 100u + s * 4u / 100u,
             s * 40u / 100u, 2u, 0x2FE38Au);
        u32 ky0 = y + s * 38u / 100u;
        u32 kh = s * 14u / 100u;
        u32 kw = s * 18u / 100u;
        for (u32 r = 0; r < 3u && ky0 + r * (kh + 2u) + kh < y + s - 3u; r++) {
            for (u32 c = 0; c < 3u; c++) {
                u32 col = (c == 2u) ? 0x3A6EA8u : 0x3A4654u;
                ud_r(x, y, s * 14u / 100u + c * (kw + 2u),
                     ky0 - y + r * (kh + 2u), kw, kh, col);
            }
        }
        break;
    }
    case UI_ICON_SETTINGS: {
        /* Шестерня: обод, 8 зубьев, ступица с отверстием. */
        u32 cx = x + s / 2u;
        u32 cy = y + s / 2u;
        u32 r = s * 26u / 100u;
        u32 d = s * 9u / 100u + 1u;
        u32 body = 0x9AAEC8u;
        u32 edge = 0x6E86A4u;
        /* Обод: внешняя кайма темнее — объём. */
        ud_fill_ellipse((s32)cx, (s32)cy, r + d, r + d, edge);
        ud_fill_ellipse((s32)cx, (s32)cy, r, r, body);
        /* Зубья: 4 осевых (прямоугольники) + 4 диагональных (диски). */
        (void)fb_fill_rect(cx - d / 2u, cy - r - 2u * d, d, 2u * d, edge);
        (void)fb_fill_rect(cx - d / 2u, cy - r - 2u * d + 1u, d, 2u * d - 1u,
                           body);
        (void)fb_fill_rect(cx - d / 2u, cy + r, d, 2u * d, edge);
        (void)fb_fill_rect(cx - d / 2u, cy + r + 1u, d, 2u * d - 1u, body);
        (void)fb_fill_rect(cx - r - 2u * d, cy - d / 2u, 2u * d, d, edge);
        (void)fb_fill_rect(cx - r - 2u * d + 1u, cy - d / 2u, 2u * d - 1u, d,
                           body);
        (void)fb_fill_rect(cx + r, cy - d / 2u, 2u * d, d, edge);
        (void)fb_fill_rect(cx + r + 1u, cy - d / 2u, 2u * d - 1u, d, body);
        s32 dg = (s32)(r * 7u / 10u);
        ud_fill_ellipse((s32)cx + dg, (s32)cy + dg, d, d, edge);
        ud_fill_ellipse((s32)cx + dg, (s32)cy + dg, d - 1u, d - 1u, body);
        ud_fill_ellipse((s32)cx + dg, (s32)cy - dg, d, d, edge);
        ud_fill_ellipse((s32)cx + dg, (s32)cy - dg, d - 1u, d - 1u, body);
        ud_fill_ellipse((s32)cx - dg, (s32)cy + dg, d, d, edge);
        ud_fill_ellipse((s32)cx - dg, (s32)cy + dg, d - 1u, d - 1u, body);
        ud_fill_ellipse((s32)cx - dg, (s32)cy - dg, d, d, edge);
        ud_fill_ellipse((s32)cx - dg, (s32)cy - dg, d - 1u, d - 1u, body);
        /* Ступица: тёмное отверстие + светлый ободок. */
        ud_fill_ellipse((s32)cx, (s32)cy, r * 52u / 100u, r * 52u / 100u,
                        0x6E86A4u);
        ud_fill_ellipse((s32)cx, (s32)cy, r * 40u / 100u, r * 40u / 100u,
                        0x10151Du);
        break;
    }
    default:
        break;
    }
}

void uidraw_disc(u32 cx, u32 cy, u32 r, u32 col)
{
    ud_fill_ellipse((s32)cx, (s32)cy, r, r, col);
}

void uidraw_logo(u32 x, u32 y, u32 size)
{
    u32 s = size;
    if (s < 10u) {
        return;
    }
    /* Тёмная скруглённая плитка (вырезы по углам 2px). */
    (void)fb_fill_rect(x + 2u, y, s - 4u, s, 0x0D1826u);
    (void)fb_fill_rect(x, y + 2u, s, s - 4u, 0x0D1826u);
    (void)fb_fill_rect(x, y + 2u, 1u, s - 4u, g_accent);
    (void)fb_fill_rect(x + s - 1u, y + 2u, 1u, s - 4u, g_accent);
    (void)fb_fill_rect(x + 2u, y, s - 4u, 1u, g_accent);
    (void)fb_fill_rect(x + 2u, y + s - 1u, s - 4u, 1u, g_accent);

    u32 cxx = x + s / 2u;
    u32 cyy = y + s / 2u;
    u32 r = s * 30u / 100u;

    /* Кольцо: параметрическая развёртка по x (тонкий эллипс 1px). */
    u32 rxa = s * 40u / 100u;
    u32 rya = s * 11u / 100u;
    for (s32 dx = -(s32)rxa; dx <= (s32)rxa; dx++) {
        u64 t = (u64)(dx < 0 ? -dx : dx);
        t = t * t * ((u64)rya * (u64)rya);
        u64 k = (u64)rxa * (u64)rxa;
        u32 dy = (u32)((u64)rya * ud_isqrt(k > t ? k - t : 0u) / (u64)rxa);
        (void)fb_fill_rect((u32)(cxx + dx), (u32)(cyy - (s32)dy), 1u, 1u,
                           0x9ECBEFu);
        (void)fb_fill_rect((u32)(cxx + dx), (u32)(cyy + (s32)dy), 1u, 1u,
                           0x9ECBEFu);
    }

    /* Планета. */
    ud_fill_ellipse((s32)cxx, (s32)cyy, r, r, g_accent);
    ud_fill_ellipse((s32)(cxx - r * 30u / 100u), (s32)(cyy - r * 30u / 100u),
                    r * 55u / 100u, r * 55u / 100u,
                    ud_mix(g_accent, 0xFFFFFFu, 60u));

    /* Передняя (нижняя) дуга кольца поверх планеты. */
    for (s32 dx = -(s32)rxa; dx <= (s32)rxa; dx++) {
        u64 t = (u64)(dx < 0 ? -dx : dx);
        t = t * t * ((u64)rya * (u64)rya);
        u64 k = (u64)rxa * (u64)rxa;
        u32 dy = (u32)((u64)rya * ud_isqrt(k > t ? k - t : 0u) / (u64)rxa);
        (void)fb_fill_rect((u32)(cxx + dx), (u32)(cyy + (s32)dy), 1u, 1u,
                           0x9ECBEFu);
    }

    /* Спутник Fobos — точка на кольце справа сверху. */
    u32 mx = cxx + rxa * 70u / 100u;
    u32 my = cyy - (u32)((u64)rya * 70u / 100u);
    (void)fb_fill_rect(mx, my, 2u, 2u, 0xE8EAEDu);
}

/* --- Курсор мыши --- */

void uidraw_mouse_event(const struct mouse_event *ev)
{
    if (g_ud.ready == 0) {
        return;
    }
    /* PS/2: положительная dy = вверх; экранные координаты растут вниз. */
    s32 nx = (s32)g_ud.mouse_x + ev->dx;
    s32 ny = (s32)g_ud.mouse_y - ev->dy;

    if (nx < 0) { nx = 0; }
    if (ny < 0) { ny = 0; }
    if ((u32)nx > g_ud.w - CUR_W - 1u) { nx = (s32)(g_ud.w - CUR_W - 1u); }
    if ((u32)ny > g_ud.h - CUR_H - 1u) { ny = (s32)(g_ud.h - CUR_H - 1u); }

    if ((u32)nx == g_ud.mouse_x && (u32)ny == g_ud.mouse_y &&
        ev->buttons == 0u) {
        return; /* нет изменений */
    }

    if (g_ud.deferred != 0) {
        /* TUI: рисует кадровый конвейер (erase по сохранённому фону здесь
         * РАЗРЫВНО — con_flush может перекрасить ячейки под курсором между
         * кадрами, и restore напечатал бы устаревшие пиксели — "квадрат"). */
        g_ud.mouse_x = (u32)nx;
        g_ud.mouse_y = (u32)ny;
        return;
    }

    uidraw_cursor_erase();
    g_ud.mouse_x = (u32)nx;
    g_ud.mouse_y = (u32)ny;
    uidraw_cursor_draw();
}

/* Режим кадрового курсора (TUI): событие мыши не трогает framebuffer. */
void uidraw_cursor_defer(int on)
{
    g_ud.deferred = (on != 0) ? 1 : 0;
}

/* Прямоугольник экрана, где СПРАЙТ курсора сейчас напечатан.
 * 1 = напечатан (кадр обязан перекрасить эту зону перед новым штампом). */
int uidraw_cursor_stamp_rect(u32 *out_x, u32 *out_y, u32 *out_w, u32 *out_h)
{
    if (g_ud.stamp_valid == 0) {
        return 0;
    }
    if (out_x != NULL) { *out_x = g_ud.stamp_x; }
    if (out_y != NULL) { *out_y = g_ud.stamp_y; }
    if (out_w != NULL) { *out_w = CUR_W; }
    if (out_h != NULL) { *out_h = CUR_H; }
    return 1;
}

void uidraw_mouse_pos(u32 *out_x, u32 *out_y)
{
    if (out_x != NULL) {
        *out_x = g_ud.mouse_x;
    }
    if (out_y != NULL) {
        *out_y = g_ud.mouse_y;
    }
}

u32 uidraw_mouse_buttons(void)
{
    return mouse_buttons();
}

/* Активная поверхность fb (задний буфер или VRAM) — только ЧТЕНИЕ фона;
 * все записи идут через fb-примитивы, чтобы не терять dirty-флаг present. */
static int ud_surface(u32 **base, u64 *pitch_px)
{
    *base = fb_surface_base();
    *pitch_px = fb_surface_pitch_px();
    return (*base != NULL && *pitch_px != 0u) ? 1 : 0;
}

void uidraw_cursor_draw(void)
{
    u32 *base;
    u64 pitch_px;
    if (g_ud.ready == 0 || ud_surface(&base, &pitch_px) == 0) {
        return;
    }
    /* Сохраняем фон из АКТИВНОЙ поверхности (в GUI-режиме это back buffer). */
    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            g_ud.saved_bg[y * CUR_W + x] =
                base[(u64)(g_ud.mouse_y + y) * pitch_px + (g_ud.mouse_x + x)];
        }
    }
    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            u8 v = cursor_sprite[y][x];
            if (v == 0u) {
                continue;
            }
            (void)fb_fill_rect(g_ud.mouse_x + x, g_ud.mouse_y + y, 1u, 1u,
                               (v == 1u) ? 0x000000u : 0xE8EAEDu);
        }
    }
    g_ud.stamp_valid = 1;
    g_ud.stamp_x = g_ud.mouse_x;
    g_ud.stamp_y = g_ud.mouse_y;
}

void uidraw_cursor_erase(void)
{
    u32 *base;
    u64 pitch_px;
    if (g_ud.ready == 0 || ud_surface(&base, &pitch_px) == 0) {
        return;
    }
    (void)base;
    (void)pitch_px;
    if (g_ud.stamp_valid == 0) {
        return;   /* спрайта нет — стирать нечем (FND-2f) */
    }
    /* Восстановление сохранённого фона строками через fb-примитив
     * (запись только через слой fb — иначе fb_present не узнает об изменении). */
    for (u32 y = 0; y < CUR_H; y++) {
        const u32 *row = &g_ud.saved_bg[y * CUR_W];
        for (u32 x = 0; x < CUR_W; x++) {
            (void)fb_fill_rect(g_ud.stamp_x + x, g_ud.stamp_y + y, 1u, 1u,
                               row[x]);
        }
    }
    g_ud.stamp_valid = 0;
}
