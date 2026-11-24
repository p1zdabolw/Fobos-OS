/*
 * Fobos OS — экранная оболочка рабочего стола FND-2b.
 *
 * Компоновка (относительная, от размеров GOP):
 *   - фон: вертикальный градиент (uidraw_background);
 *   - панель консоли: поля 20 px, заголовок 20 px, скругление 6 px;
 *   - статус-бар: 28 px внизу (версия | сессия | часы RTC | память |
 *     uptime | мышь);
 *   - курсор мыши: uidraw.
 *
 * Это прототип ядра-композитора: настоящий композитор с surface'ами —
 * GUI-1 (см. docs/roadmap.md).
 */
#include "ui.h"
#include "font8x8.h"
#include "uidraw.h"
#include "../version.h"
#include "../drivers/fb.h"
#include "../drivers/timer.h"
#include "../drivers/mouse.h"
#include "../drivers/rtc.h"
#include "../mm/phys.h"
#include "../sys/session.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"

#define UI_MARGIN        20u
#define UI_HEADER_H      20u
#define UI_BAR_H         28u
#define UI_BAR_GAP       12u
#define UI_PAD           10u

static const char *const UI_TAG = "gfx/ui";

struct ui_state {
    u32 w, h;
    u32 last_second;
    int ready;
};

static struct ui_state g_ui;

/* Рисование текста напрямую в fb (в обход консоли) — для панели/бара. */
static void draw_text_px(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    for (u32 i = 0; s[i] != '\0' && x + i * 8u + 8u <= g_ui.w; i++) {
        char c = s[i];
        if (c < (char)0x20 || c > (char)0x7Eu) {
            c = '.';
        }
        (void)fb_blit_glyph8(x + i * 8u, y, font8x8[c - FONT8X8_FIRST], fg, bg);
    }
}

static void draw_panel_bordered(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 border)
{
    (void)fb_fill_rect(x, y, w, h, fill);
    (void)fb_fill_rect(x, y, w, 1u, border);
    (void)fb_fill_rect(x, y + h - 1u, w, 1u, border);
    (void)fb_fill_rect(x, y, 1u, h, border);
    (void)fb_fill_rect(x + w - 1u, y, 1u, h, border);
}

static void format_clock(char *buf, u32 cap)
{
    buf[0] = '\0';
    struct rtc_time t;
    if (rtc_ready() != 0 && rtc_read_time(&t) == K_OK) {
        char full[RTC_STR_MIN];
        if (rtc_format(full, sizeof(full), &t) >= 16) {
            u32 i = 0;
            for (; i < 16u && i + 1u < cap; i++) {
                buf[i] = full[i];
            }
            buf[i] = '\0';
        }
    }
}

static void format_status_right(char *buf, u32 bufsz)
{
    struct phys_stats st = phys_get_stats();
    u64 total_mib = st.total_frames * 4ull / 1024ull;
    u64 free_mib  = st.free_frames * 4ull / 1024ull;

    u64 ms = timer_uptime_ms();
    u32 sec = (u32)((ms / 1000ull) % 60ull);
    u32 min = (u32)((ms / 60000ull) % 60ull);
    u32 hr  = (u32)(ms / 3600000ull);

    (void)ksnprintf(buf, bufsz, "mem %llu/%llu MiB | up %u:%02u:%02u",
                    (unsigned long long)free_mib, (unsigned long long)total_mib,
                    hr, min, sec);
}

void ui_statusbar_update(void)
{
    if (g_ui.ready == 0) {
        return;
    }
    u32 by = g_ui.h - UI_BAR_H;

    (void)fb_fill_rect(0, by, g_ui.w, UI_BAR_H, UI_COL_BAR);
    (void)fb_fill_rect(0, by, g_ui.w, 1u, UI_COL_BORDER);

    /* Левая часть: версия + контекст сессии. */
    char left[96];
    if (session_active() != 0) {
        (void)ksnprintf(left, sizeof(left), " Fobos OS %s | %s@%s",
                        FOBOS_VERSION_STR, session_user(), session_hostname());
    } else {
        (void)ksnprintf(left, sizeof(left), " Fobos OS %s | kernel console",
                        FOBOS_VERSION_STR);
    }
    draw_text_px(UI_MARGIN / 2u, by + (UI_BAR_H - 8u) / 2u,
                 left, UI_COL_TX_DIM, UI_COL_BAR);

    /* Правая часть: часы + память/uptime/мышь. */
    char right[96];
    format_status_right(right, sizeof(right));

    char clock[24];
    format_clock(clock, sizeof(clock));

    char mpos[24];
    u32 mx = 0, my = 0;
    uidraw_mouse_pos(&mx, &my);
    (void)ksnprintf(mpos, sizeof(mpos), " | %llu,%llu",
                    (unsigned long long)mx, (unsigned long long)my);

    char full[160];
    if (clock[0] != '\0') {
        (void)ksnprintf(full, sizeof(full), "%s | %s%s", clock, right, mpos);
    } else {
        (void)ksnprintf(full, sizeof(full), "%s%s", right, mpos);
    }

    u32 len = 0;
    while (full[len] != '\0') {
        len++;
    }
    u32 text_w = len * 8u + 4u;
    u32 x = (g_ui.w > text_w + UI_MARGIN) ? (g_ui.w - text_w - UI_MARGIN / 2u) : 0u;
    draw_text_px(x, by + (UI_BAR_H - 8u) / 2u, full, UI_COL_TX_DIM, UI_COL_BAR);
}

int ui_statusbar_due(void)
{
    if (g_ui.ready == 0) {
        return 0;
    }
    /* Обновление раз в секунду и по часам RTC (реальное время). */
    u32 sec = (u32)(timer_uptime_ms() / 1000ull);
    if (sec != g_ui.last_second) {
        g_ui.last_second = sec;
        return 1;
    }
    return 0;
}

/* Геометрия консольной области TUI (общая для ui_init и сброса сессии):
 * поля 20 px, заголовок 20 px, статус-бар 28 px внизу. */
static void ui_console_geom(u32 w, u32 h, u32 *px, u32 *py, u32 *pw, u32 *ph)
{
    *px = UI_MARGIN + 1u + UI_PAD;
    *py = UI_MARGIN + 1u + UI_HEADER_H + 4u + UI_PAD;
    *pw = w - 2u * (UI_MARGIN + 1u + UI_PAD);
    *ph = h - UI_MARGIN - UI_HEADER_H - UI_PAD - UI_BAR_H -
          UI_BAR_GAP - UI_MARGIN - 2u;
}

err_t ui_init(void)
{
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    if (w < 640u || h < 400u) {
        klog(KLOG_WARN, UI_TAG, "экран %ux%u мал для UI FND-2b (мин. 640x400)", w, h);
    }

    g_ui.w = w;
    g_ui.h = h;
    g_ui.last_second = 0xFFFFFFFFu;
    g_ui.ready = 1;

    ui_redraw_all();

    /* FND-2f: ПРИВЯЗКА, а не сброс. con_resize при готовой консоли
     * сохраняет контент (переключения TUI<->GUI больше не стирают
     * историю); при первом старте con_resize сам вызовет con_init. */
    u32 px, py, pw, ph;
    ui_console_geom(w, h, &px, &py, &pw, &ph);
    err_t crc = con_resize(px, py, pw, ph);
    if (crc != K_OK) {
        klog(KLOG_ERROR, UI_TAG, "con_resize: %s", k_strerror(crc));
        return crc;
    }

    klog(KLOG_INFO, UI_TAG,
         "UI: экран %ux%u, консоль %ux%u ячеек",
         w, h, con_cols(), con_rows());
    return K_OK;
}

/* Новая сессия — чистая консоль (dsk_init перед dsk_switch). История
 * предыдущей сессии не должна смешиваться с новой. */
void ui_console_session_reset(void)
{
    u32 w = 0, h = 0;
    if (fb_get_dims(&w, &h) != K_OK) {
        return;
    }
    u32 px, py, pw, ph;
    ui_console_geom(w, h, &px, &py, &pw, &ph);
    (void)con_init(px, py, pw, ph);
}

void ui_redraw_all(void)
{
    if (g_ui.ready == 0) {
        return;
    }

    uidraw_cursor_erase();
    uidraw_background();

    u32 px = UI_MARGIN;
    u32 py = UI_MARGIN;
    u32 pw = g_ui.w - 2u * UI_MARGIN;
    u32 ph = g_ui.h - UI_MARGIN - (UI_BAR_H + UI_BAR_GAP) - UI_MARGIN;

    draw_panel_bordered(px, py, pw, ph, UI_COL_PANEL, UI_COL_BORDER);

    /* Заголовок панели. */
    (void)fb_fill_rect(px + 1u, py + 1u, pw - 2u, UI_HEADER_H, UI_COL_BAR);
    (void)fb_fill_rect(px + 1u, py + 1u + UI_HEADER_H, pw - 2u, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(px + 6u, py + 7u, 6u, 6u, UI_COL_ACCENT);

    char title[96];
    if (session_active() != 0) {
        (void)ksnprintf(title, sizeof(title),
                        "FOBOS CONSOLE - terminal (%s@%s)",
                        session_user(), session_hostname());
    } else {
        (void)ksnprintf(title, sizeof(title),
                        "FOBOS CONSOLE - kernel shell (%s)", FOBOS_VERSION_STR);
    }
    draw_text_px(px + 20u, py + 6u, title, UI_COL_HEADER_TX, UI_COL_BAR);

    /* Статус-бар и курсор. */
    ui_statusbar_update();
    uidraw_cursor_draw();
    fb_present();   /* TUI-панель строится вне main_loop (ui_init/fbtest) */
}

void ui_mouse_event(const struct mouse_event *ev)
{
    if (g_ui.ready == 0) {
        return;
    }
    uidraw_mouse_event(ev);
}

void ui_mouse_pos(u32 *out_x, u32 *out_y)
{
    uidraw_mouse_pos(out_x, out_y);
}
