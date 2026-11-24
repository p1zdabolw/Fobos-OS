/*
 * Fobos OS — рабочий стол TUI/GUI (реализация, FND-2e «Shell 2.0»).
 *
 * Слои кадра (снизу вверх):
 *   обои (uidraw_background, вариант из настроек)
 *   -> иконки рабочего стола
 *   -> окна: нефокусные, затем фокусное (рамка + клип + контент)
 *   -> панель задач
 *   -> Start-меню / всплывающее меню
 *   -> курсор мыши
 *   -> fb_present() (один готовый кадр в VRAM).
 *
 * Ключевые решения FND-2e:
 *  - клиппинг содержимого окон на уровне рендера (fb_clip_set): класс
 *    багов «текст/контент вылезает за рамку» закрыт по построению;
 *  - resize окон за 8 зон (4 края + 4 угла), EDGE_PX зон;
 *  - таскбар: центр, только иконки, закрепления из fobos.cfg;
 *  - Start-меню с поиском; маршрутизация клавиатуры через dsk_kbd_event.
 *
 * Взаимодействие: клик открывает иконки рабочего стола по двойному
 * клику (порог 500 мс), окна — одиночным.
 */
#include "dskpriv.h"

#include <stddef.h>

#include "font8x8.h"
#include "uidraw.h"
#include "ui.h"
#include "console.h"
#include "shell.h"
#include "../version.h"
#include "../drivers/fb.h"
#include "../drivers/timer.h"
#include "../drivers/rtc.h"
#include "../drivers/mouse.h"
#include "../fs/blk.h"
#include "../fs/fat32.h"
#include "../sys/session.h"
#include "../sys/power.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"
#include "../../common/cfglite.h"

#define D_TAG "gfx/desktop"

/* Имена приложений: рабочий стол, пины, таскбар, shell — один словарь. */
const char *const g_app_names[APP_COUNT] = {
    "cmd", "explorer", "about", "viewer", "notepad", "calc", "settings"
};

/* Иконки uidraw, соответствующие приложениям (индексы совпадают). */
static const u8 g_app_icons[APP_COUNT] = {
    UI_ICON_CMD, UI_ICON_EXPLORER, UI_ICON_ABOUT, UI_ICON_VIEWER,
    UI_ICON_NOTEPAD, UI_ICON_CALC, UI_ICON_SETTINGS
};

struct dsk_state g_dsk;

/* Залоченный режим: -1 = нет строки mode= в конфиге. */
static int s_locked_mode = -1;

/* Контекст всплывающего меню (какое приложение его вызвало). */
static u8 s_menu_app;
static void menu_exec(u32 action);

/* Позиция кнопки питания в Start (для открытия меню). */
static u32 s_power_x, s_power_y, s_power_w, s_power_h;

/* ------------------------------------------------------------------ */
/* Геометрия окон                                                      */
/* ------------------------------------------------------------------ */

void dsk_win_default(u8 app, struct dsk_win *w)
{
    u32 sw = g_dsk.w, sh = g_dsk.h;
    switch (app) {
    case APP_CMD:
        w->x = 96; w->y = 48; w->w = 560; w->h = 360;
        break;
    case APP_EXPL:
        w->x = (sw > 700u) ? sw - 500u : 120u;
        w->y = 48; w->w = 460; w->h = 340;
        break;
    case APP_ABOUT:
        w->x = (sw > 520u) ? (sw - 480u) / 2u : 60u;
        w->y = (sh > 420u) ? (sh - TB_H - 300u) / 2u : 60u;
        w->w = 480; w->h = 300;
        break;
    case APP_VIEW:
        w->x = (sw > 580u) ? (sw - 540u) / 2u : 40u;
        w->y = (sh > 480u) ? (sh - TB_H - 400u) / 2u : 40u;
        w->w = 540; w->h = 400;
        break;
    case APP_NOTEPAD:
        w->x = (sw > 620u) ? (sw - 580u) / 2u : 50u;
        w->y = (sh > 500u) ? (sh - TB_H - 420u) / 2u : 40u;
        w->w = 580; w->h = 420;
        break;
    case APP_CALC:
        w->w = 264; w->h = 300;
        w->x = (sw > 320u) ? (sw - 264u) / 2u : 40u;
        w->y = (sh > 380u) ? (sh - TB_H - 300u) / 2u : 40u;
        break;
    case APP_SET:
        w->x = (sw > 700u) ? (sw - 640u) / 2u : 40u;
        w->y = (sh > 500u) ? (sh - TB_H - 400u) / 2u : 40u;
        w->w = 640; w->h = 400;
        break;
    default:
        w->x = 40; w->y = 40; w->w = 320; w->h = 240;
        break;
    }
    w->resizable = 1;
    /* Не вылезать за экран/панель задач. */
    if (w->x + w->w > sw) { w->x = (sw > w->w) ? sw - w->w : 0u; }
    if (w->y + w->h > sh - TB_H) {
        w->y = (sh - TB_H > w->h) ? sh - TB_H - w->h : 0u;
    }
}

void dsk_win_content(const struct dsk_win *w, u32 *cx, u32 *cy,
                     u32 *cw, u32 *ch)
{
    *cx = w->x + 2u;
    *cy = w->y + TITLE_H + 2u;
    *cw = (w->w > 4u) ? w->w - 4u : 0u;
    *ch = (w->h > TITLE_H + 4u) ? w->h - TITLE_H - 4u : 0u;
}

u32 dsk_edge_at(const struct dsk_win *w, u32 mx, u32 my)
{
    u32 mask = 0;
    if (w->resizable == 0) {
        return 0;
    }
    if (uidraw_rect_hit(w->x, w->y, w->w, w->h, mx, my) == 0) {
        return 0;
    }
    if (mx < w->x + EDGE_PX)                 { mask |= EDGE_L; }
    if (mx >= w->x + w->w - EDGE_PX)         { mask |= EDGE_R; }
    if (my < w->y + EDGE_PX)                 { mask |= EDGE_T; }
    if (my >= w->y + w->h - EDGE_PX)         { mask |= EDGE_B; }
    return mask;
}

void dsk_draw_frame(u8 app)
{
    const struct dsk_win *w = &g_dsk.win[app];
    int focused = (app == g_dsk.focus);
    u32 border = (focused != 0) ? uidraw_accent() : UI_COL_BORDER;

    (void)fb_fill_rect(w->x, w->y, w->w, w->h, UI_COL_PANEL);
    (void)fb_fill_rect(w->x, w->y, w->w, 1u, border);
    (void)fb_fill_rect(w->x, w->y + w->h - 1u, w->w, 1u, border);
    (void)fb_fill_rect(w->x, w->y, 1u, w->h, border);
    (void)fb_fill_rect(w->x + w->w - 1u, w->y, 1u, w->h, border);

    /* Заголовок. */
    (void)fb_fill_rect(w->x + 1u, w->y + 1u, w->w - 2u, TITLE_H, UI_COL_BAR);
    (void)fb_fill_rect(w->x + 1u, w->y + 1u + TITLE_H, w->w - 2u, 1u, border);
    (void)fb_fill_rect(w->x + 6u, w->y + 8u, 4u, 4u, uidraw_accent());

    char title[72];
    static const char *const titles[APP_COUNT] = {
        "cmd - fobos terminal", "explorer - fat32", "about fobos os",
        "viewer", "notepad", "calculator", "settings"
    };
    if (app == APP_VIEW) {
        extern void dskapps_view_title(char *out, u32 cap);
        dskapps_view_title(title, sizeof(title));
    } else {
        (void)ksnprintf(title, sizeof(title), "%s", titles[app]);
    }
    /* Заголовок ВСЕГДА в границах окна: fit с "..", правее — кнопка. */
    u32 close_zone = 22u;
    (void)uidraw_text_fit(w->x + 16u, w->y + (TITLE_H - 8u) / 2u + 1u,
                          w->w - 16u - close_zone - 4u, title,
                          UI_COL_HEADER_TX, UI_COL_BAR);

    /* Кнопка закрытия 12x12. */
    u32 cx = w->x + w->w - 16u;
    u32 cy = w->y + (TITLE_H - 12u) / 2u + 1u;
    (void)fb_fill_rect(cx, cy, 12u, 12u, UI_COL_PANEL);
    (void)fb_fill_rect(cx, cy, 12u, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(cx, cy + 11u, 12u, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(cx, cy, 1u, 12u, UI_COL_BORDER);
    (void)fb_fill_rect(cx + 11u, cy, 1u, 12u, UI_COL_BORDER);
    uidraw_text(cx + 2u, cy + 2u, "x", UI_COL_TX, UI_COL_PANEL);
}

/* ------------------------------------------------------------------ */
/* Всплывающее меню (одно на экран)                                    */
/* ------------------------------------------------------------------ */

struct menu_item {
    const char *label;
};

/* Подписи действий; часть динамические — через статические буферы. */
static char s_menu_buf[8][24];

static void menu_build_items(const u32 *actions, u32 count,
                             struct menu_item *items)
{
    for (u32 i = 0; i < count && i < 8u; i++) {
        switch (actions[i]) {
        case MENU_PIN:      items[i].label = "Pin to taskbar";  break;
        case MENU_UNPIN:    items[i].label = "Unpin";           break;
        case MENU_CLOSE:    items[i].label = "Close";           break;
        case MENU_REFRESH:  items[i].label = "Refresh";         break;
        case MENU_DISPLAY:  items[i].label = "Display settings"; break;
        case MENU_PERSONAL: items[i].label = "Personalize";     break;
        case MENU_TERMINAL: items[i].label = "Terminal";        break;
        case MENU_ABOUT:    items[i].label = "About Fobos";      break;
        case MENU_LOGOUT:   items[i].label = "Log out";         break;
        case MENU_REBOOT:   items[i].label = "Reboot";          break;
        case MENU_POWEROFF: items[i].label = "Power off";       break;
        case MENU_AMNESIA:
            (void)ksnprintf(s_menu_buf[i], sizeof(s_menu_buf[i]),
                            "Amnesia: %s",
                            (blk_amnesia_active() != 0) ? "off" : "on");
            items[i].label = s_menu_buf[i];
            break;
        default:            items[i].label = "?";               break;
        }
    }
}

#define MENU_W 150u
#define MENU_ITEM_H 18u

void dsk_menu_open(u32 x, u32 y, const u32 *actions, u32 count)
{
    if (count > 8u) {
        count = 8u;
    }
    g_dsk.menu_open = 1;
    g_dsk.menu_count = count;
    for (u32 i = 0; i < count; i++) {
        g_dsk.menu_action[i] = actions[i];
    }
    u32 mh = count * MENU_ITEM_H + 6u;
    if (x + MENU_W > g_dsk.w) { x = (g_dsk.w > MENU_W) ? g_dsk.w - MENU_W : 0u; }
    if (y + mh > g_dsk.h)     { y = (g_dsk.h > mh) ? g_dsk.h - mh : 0u; }
    g_dsk.menu_x = x;
    g_dsk.menu_y = y;
}

void dsk_menu_close(void)
{
    g_dsk.menu_open = 0;
    g_dsk.menu_count = 0;
}

void dsk_menu_draw(void)
{
    if (g_dsk.menu_open == 0 || g_dsk.menu_count == 0) {
        return;
    }
    struct menu_item items[8];
    menu_build_items(g_dsk.menu_action, g_dsk.menu_count, items);

    u32 mh = g_dsk.menu_count * MENU_ITEM_H + 6u;
    (void)fb_fill_rect(g_dsk.menu_x, g_dsk.menu_y, MENU_W, mh, 0x232B38u);
    (void)fb_fill_rect(g_dsk.menu_x, g_dsk.menu_y, MENU_W, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(g_dsk.menu_x, g_dsk.menu_y + mh - 1u, MENU_W, 1u,
                       UI_COL_BORDER);
    (void)fb_fill_rect(g_dsk.menu_x, g_dsk.menu_y, 1u, mh, UI_COL_BORDER);
    (void)fb_fill_rect(g_dsk.menu_x + MENU_W - 1u, g_dsk.menu_y, 1u, mh,
                       UI_COL_BORDER);

    u32 mx, my;
    uidraw_mouse_pos(&mx, &my);
    for (u32 i = 0; i < g_dsk.menu_count; i++) {
        u32 iy = g_dsk.menu_y + 3u + i * MENU_ITEM_H;
        u32 fill = UI_COL_PANEL;
        if (uidraw_rect_hit(g_dsk.menu_x + 1u, iy, MENU_W - 2u, MENU_ITEM_H,
                            mx, my) != 0) {
            fill = 0x31405Au;   /* hover */
        }
        (void)fb_fill_rect(g_dsk.menu_x + 1u, iy, MENU_W - 2u, MENU_ITEM_H,
                           fill);
        uidraw_text(g_dsk.menu_x + 8u, iy + (MENU_ITEM_H - 8u) / 2u,
                    items[i].label, UI_COL_TX, fill);
    }
}

int dsk_menu_click(u32 mx, u32 my)
{
    if (g_dsk.menu_open == 0) {
        return 0;
    }
    u32 mh = g_dsk.menu_count * MENU_ITEM_H + 6u;
    if (uidraw_rect_hit(g_dsk.menu_x, g_dsk.menu_y, MENU_W, mh, mx, my) == 0) {
        dsk_menu_close();
        return 1;
    }
    u32 sel = (my - g_dsk.menu_y - 3u) / MENU_ITEM_H;
    u32 action = (sel < g_dsk.menu_count) ? g_dsk.menu_action[sel] : MENU_NONE;
    dsk_menu_close();
    menu_exec(action);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Панель задач: центр — [Пуск][закрепления/запущенные], справа — часы  */
/* ------------------------------------------------------------------ */

#define TB_BTN     32u   /* сторона кнопки на панели */
#define TB_ICON    22u   /* сторона иконки внутри кнопки */
#define TB_GAP      2u   /* зазор между кнопками */
#define TB_MARGIN  12u   /* поле группы от краёв */

/* Кнопка Пуск — первая в центральной группе. */
static u32 s_start_btn_x, s_start_btn_y;

int dsk_start_is_open(void)
{
    return g_dsk.menu_open == 0 ? g_dsk.start_open : 0;
}

/* Собирает список приложений центральной группы в порядке отображения:
 * сначала закреплённые (в порядке pins), затем запущенные незакреплённые.
 * Возвращает число кнопок; out_app — id приложения каждой кнопки. */
static u32 taskbar_buttons(u8 *out_app, u32 max)
{
    u32 n = 0;
    for (u32 p = 0; p < g_dsk.cfg_pin_count && n < max; p++) {
        int app = -1;
        for (u32 i = 0; i < APP_COUNT; i++) {
            if (strcmp(g_app_names[i], g_dsk.cfg_pins[p]) == 0) {
                app = (int)i;
                break;
            }
        }
        if (app >= 0 && (u32)app != APP_VIEW) {
            out_app[n++] = (u8)app;
        }
    }
    for (u32 i = 0; i < APP_COUNT && n < max; i++) {
        if (i == APP_VIEW || g_dsk.win[i].open == 0) {
            continue;
        }
        int dup = 0;
        for (u32 k = 0; k < n; k++) {
            if (out_app[k] == (u8)i) { dup = 1; break; }
        }
        if (dup == 0) {
            out_app[n++] = (u8)i;
        }
    }
    return n;
}

/* Геометрия центральной группы; для каждой кнопки — x. */
static u32 taskbar_layout(u8 *out_app, u32 max, u32 *out_x, u32 *out_y,
                          u32 *group_w)
{
    u32 n = taskbar_buttons(out_app, max);
    u32 total = TB_BTN + TB_GAP + n * (TB_BTN + TB_GAP);
    u32 gx = (g_dsk.w > total) ? (g_dsk.w - total) / 2u : 0u;
    u32 gy = g_dsk.h - TB_H + (TB_H - TB_BTN) / 2u;
    if (out_x != NULL) {
        for (u32 i = 0; i < n; i++) {
            out_x[i] = gx + (TB_BTN + TB_GAP) + i * (TB_BTN + TB_GAP);
        }
    }
    s_start_btn_x = gx;
    s_start_btn_y = gy;
    if (out_y != NULL) {
        *out_y = gy;
    }
    if (group_w != NULL) {
        *group_w = total;
    }
    return n;
}

void dsk_taskbar_draw(void)
{
    u32 by = g_dsk.h - TB_H;
    (void)fb_fill_rect(0, by, g_dsk.w, TB_H, UI_COL_BAR);
    (void)fb_fill_rect(0, by, g_dsk.w, 1u, UI_COL_BORDER);

    u32 mx, my;
    uidraw_mouse_pos(&mx, &my);

    /* Центральная группа. */
    u8 apps[APP_COUNT + 1];
    u32 xs[APP_COUNT + 1];
    u32 gy;
    u32 n = taskbar_layout(apps, APP_COUNT + 1u, xs, &gy, NULL);

    /* Кнопка Пуск. */
    u32 hover = uidraw_rect_hit(s_start_btn_x, gy, TB_BTN, TB_BTN, mx, my);
    u32 fill = (g_dsk.start_open != 0) ? 0x273448u
               : (hover != 0) ? 0x212C3Cu : UI_COL_BAR;
    (void)fb_fill_rect(s_start_btn_x, gy, TB_BTN, TB_BTN, fill);
    uidraw_logo(s_start_btn_x + (TB_BTN - TB_ICON) / 2u,
                gy + (TB_BTN - TB_ICON) / 2u, TB_ICON);

    /* Кнопки приложений: только иконки + pill-индикатор запуска. */
    for (u32 i = 0; i < n; i++) {
        u8 app = apps[i];
        u32 bx = xs[i];
        int open = (g_dsk.win[app].open != 0);
        int focused = (open != 0 && app == g_dsk.focus);
        hover = uidraw_rect_hit(bx, gy, TB_BTN, TB_BTN, mx, my);
        fill = (focused != 0) ? 0x273448u : (hover != 0) ? 0x212C3Cu
                                                         : UI_COL_BAR;
        (void)fb_fill_rect(bx, gy, TB_BTN, TB_BTN, fill);
        uidraw_app_icon(g_app_icons[app], bx + (TB_BTN - TB_ICON) / 2u,
                        gy + (TB_BTN - TB_ICON) / 2u, TB_ICON,
                        (focused != 0 || hover != 0) ? 1 : 0);
        if (open != 0) {
            /* Индикатор запуска: акцент у фокусного, серый у остальных. */
            u32 pill = (focused != 0) ? uidraw_accent() : 0x5A6878u;
            (void)fb_fill_rect(bx + (TB_BTN - 14u) / 2u, gy + TB_BTN - 3u,
                               14u, 2u, pill);
        }
    }

    /* Правая часть: AMNESIA + часы/дата (две строки). */
    char tstr[RTC_STR_MIN];
    struct rtc_time t;
    tstr[0] = '\0';
    if (rtc_ready() != 0 && rtc_read_time(&t) == K_OK) {
        (void)rtc_format(tstr, sizeof(tstr), &t);
    }

    u32 rx = g_dsk.w - 8u;
    if (tstr[0] != '\0') {
        /* tstr = "YYYY-MM-DD HH:MM:SS" — делим на дату и время. */
        char date[12];
        char hm[10];
        for (u32 i = 0; i < 10u; i++) { date[i] = tstr[i]; }
        date[10] = '\0';
        hm[0] = tstr[11]; hm[1] = tstr[12]; hm[2] = ':';
        hm[3] = tstr[14]; hm[4] = tstr[15]; hm[5] = '\0';
        if (g_dsk.cfg_time24 == 0) {
            u32 h24 = (u32)(tstr[11] - '0') * 10u + (u32)(tstr[12] - '0');
            const char *ap = (h24 >= 12u) ? "PM" : "AM";
            u32 h12 = h24 % 12u;
            if (h12 == 0u) { h12 = 12u; }
            (void)ksnprintf(hm, sizeof(hm), "%u:%c%c %s", h12,
                            tstr[14], tstr[15], ap);
        }
        if (blk_amnesia_active() != 0) {
            rx -= 9u * 8u;
            uidraw_text(rx, by + (TB_H - 16u) / 2u, "[AMNESIA]",
                        UI_COL_ERR, UI_COL_BAR);
        }
        uidraw_text_fit(rx - 8u * 8u, by + (TB_H - 16u) / 2u, 8u * 8u, hm,
                        UI_COL_TX, UI_COL_BAR);
        uidraw_text_fit(rx - 8u * 10u, by + (TB_H - 16u) / 2u + 10u,
                        10u * 8u, date, UI_COL_TX_DIM, UI_COL_BAR);
    } else {
        uidraw_text_fit(rx - 10u * 8u, by + (TB_H - 8u) / 2u, 10u * 8u,
                        "--:--", UI_COL_TX_DIM, UI_COL_BAR);
    }
}

int dsk_taskbar_click(u32 mx, u32 my, int right_click)
{
    u32 by = g_dsk.h - TB_H;
    if (my < by) {
        return 0;
    }

    u8 apps[APP_COUNT + 1];
    u32 xs[APP_COUNT + 1];
    u32 gy;
    u32 n = taskbar_layout(apps, APP_COUNT + 1u, xs, &gy, NULL);

    /* Пуск. */
    if (uidraw_rect_hit(s_start_btn_x, gy, TB_BTN, TB_BTN, mx, my) != 0) {
        if (right_click == 0) {
            dsk_start_toggle();
        }
        return 1;
    }

    for (u32 i = 0; i < n; i++) {
        u8 app = apps[i];
        if (uidraw_rect_hit(xs[i], gy, TB_BTN, TB_BTN, mx, my) != 0) {
            if (right_click != 0) {
                s_menu_app = app;
                u32 actions[2];
                u32 count = 0;
                actions[count++] = dsk_pin_index(app) >= 0 ? MENU_UNPIN
                                                           : MENU_PIN;
                if (g_dsk.win[app].open != 0) {
                    actions[count++] = MENU_CLOSE;
                }
                dsk_menu_open(xs[i] - (MENU_W - TB_BTN) / 2u, gy - count * MENU_ITEM_H - 6u,
                              actions, count);
                return 1;
            }
            /* Клик: скрыть/фокус/открыть. */
            if (g_dsk.win[app].open != 0) {
                if (g_dsk.focus == app) {
                    dsk_app_close(app);
                } else {
                    g_dsk.focus = app;
                }
            } else {
                dsk_app_open(app);
            }
            return 1;
        }
    }
    return 1;   /* клик в свободное место панели — просто съедаем */
}

/* ------------------------------------------------------------------ */
/* Start-меню                                                          */
/* ------------------------------------------------------------------ */

#define START_W   520u
#define START_H   380u
#define START_SEARCH_H 22u
#define TILE_W    76u
#define TILE_H    68u
#define TILE_COLS 6u
#define TILE_ROWS 2u

void dsk_start_toggle(void)
{
    g_dsk.start_open = (g_dsk.start_open != 0) ? 0 : 1;
    if (g_dsk.start_open != 0) {
        g_dsk.start_search[0] = '\0';
        g_dsk.start_sel = 0;
        dsk_menu_close();
    }
    dsk_request_redraw();   /* смена состояния UI = новый кадр */
    klog(KLOG_INFO, D_TAG, "start menu: %s",
         (g_dsk.start_open != 0) ? "open" : "closed");
}

/* Позиция/размер панели Start. */
static void start_rect(u32 *x, u32 *y)
{
    u32 px = (g_dsk.w > START_W) ? (g_dsk.w - START_W) / 2u : 0u;
    u32 py = (g_dsk.h > START_H + TB_H) ? g_dsk.h - TB_H - START_H - 6u : 0u;
    *x = px;
    *y = py;
}

/* Совпадение имени с поиском (подстрока, без регистра; strstr в ядре нет). */
static int cl_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int str_sub(const char *hay, const char *needle)
{
    if (needle[0] == '\0') {
        return 1;
    }
    for (u32 i = 0; hay[i] != '\0'; i++) {
        u32 j = 0;
        while (needle[j] != '\0' && cl_lower(hay[i + j]) == cl_lower(needle[j])) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int name_matches(const char *name, const char *q)
{
    if (q[0] == '\0') {
        return 1;
    }
    return str_sub(name, q);
}

/* Текущий список приложений в меню (поиск/все): out_idx — id. */
static u32 start_results(u32 *out_idx, u32 max)
{
    char q[APP_NAME_MAX];
    u32 qi = 0;
    for (; g_dsk.start_search[qi] != '\0' && qi + 1u < sizeof(q); qi++) {
        q[qi] = (char)cl_lower(g_dsk.start_search[qi]);
    }
    q[qi] = '\0';

    u32 n = 0;
    for (u32 i = 0; i < APP_COUNT && n < max; i++) {
        if (i == APP_VIEW) {
            continue;   /* viewer — не самостоятельное приложение */
        }
        if (name_matches(g_app_names[i], q) != 0) {
            out_idx[n++] = i;
        }
    }
    return n;
}

void dsk_start_draw(void)
{
    if (g_dsk.start_open == 0) {
        return;
    }
    u32 x, y;
    start_rect(&x, &y);

    /* Панель с лёгкой тенью (кайма 2px темнее). */
    (void)fb_fill_rect(x + 2u, y + 2u, START_W, START_H, 0x0A0E14u);
    (void)fb_fill_rect(x, y, START_W, START_H, 0x1C2431u);
    (void)fb_fill_rect(x, y, START_W, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(x, y + START_H - 1u, START_W, 1u, UI_COL_BORDER);
    (void)fb_fill_rect(x, y, 1u, START_H, UI_COL_BORDER);
    (void)fb_fill_rect(x + START_W - 1u, y, 1u, START_H, UI_COL_BORDER);

    /* Поиск. */
    uidraw_input_field(x + 12u, y + 10u, START_W - 24u, g_dsk.start_search,
                       (u32)strlen(g_dsk.start_search), 1, 0);

    u32 grid_y = y + 10u + START_SEARCH_H + 10u;
    u32 results[APP_COUNT];
    u32 n = start_results(results, APP_COUNT);

    if (g_dsk.start_search[0] == '\0') {
        uidraw_text(x + 14u, grid_y, "Pinned", UI_COL_HEADER_TX, 0x1C2431u);
        grid_y += 14u;
        /* Плитки: иконка + подпись, до 2 строк. */
        for (u32 r = 0; r < TILE_ROWS; r++) {
            for (u32 c = 0; c < TILE_COLS; c++) {
                u32 i = r * TILE_COLS + c;
                if (i >= n) {
                    break;
                }
                u8 app = (u8)results[i];
                u32 tx = x + 14u + c * (TILE_W + 6u);
                u32 ty = grid_y + r * (TILE_H + 6u);
                u32 mx, my;
                uidraw_mouse_pos(&mx, &my);
                u32 fill = 0x222B3Au;
                if (uidraw_rect_hit(tx, ty, TILE_W, TILE_H, mx, my) != 0) {
                    fill = 0x2C3A52u;
                }
                (void)fb_fill_rect(tx, ty, TILE_W, TILE_H, fill);
                uidraw_app_icon(g_app_icons[app], tx + (TILE_W - 30u) / 2u,
                                ty + 8u, 30u, 0);
                uidraw_text_fit(tx + 4u, ty + TILE_H - 14u, TILE_W - 8u,
                                g_app_names[app], UI_COL_TX, fill);
            }
        }
    } else {
        /* Результаты поиска списком. */
        for (u32 i = 0; i < n && i < 8u; i++) {
            u8 app = (u8)results[i];
            u32 ry = grid_y + i * 26u;
            u32 fill = ((int)i == g_dsk.start_sel) ? 0x2C3A52u : 0x1C2431u;
            (void)fb_fill_rect(x + 12u, ry, START_W - 24u, 24u, fill);
            uidraw_app_icon(g_app_icons[app], x + 18u, ry + 3u, 18u, 0);
            uidraw_text(x + 44u, ry + 8u, g_app_names[app], UI_COL_TX, fill);
        }
        if (n == 0u) {
            uidraw_text(x + 14u, grid_y, "No matches", UI_COL_TX_DIM,
                        0x1C2431u);
        }
    }

    /* Нижняя панель: пользователь + питание. */
    u32 byy = y + START_H - 44u;
    (void)fb_fill_rect(x + 1u, byy, START_W - 2u, 43u, 0x161D29u);
    (void)fb_fill_rect(x + 1u, byy, START_W - 2u, 1u, UI_COL_BORDER);

    /* Аватар-диск + имя пользователя. */
    u32 av_x = x + 16u;
    u32 av_y = byy + (43u - 26u) / 2u;
    uidraw_disc(av_x + 13u, av_y + 13u, 13u, uidraw_accent());
    char uname[24];
    if (session_active() != 0) {
        (void)ksnprintf(uname, sizeof(uname), "%s", session_user());
    } else {
        (void)ksnprintf(uname, sizeof(uname), "user");
    }
    uidraw_text_fit(av_x + 26u, byy + (43u - 8u) / 2u, 140u, uname,
                    UI_COL_TX, 0x161D29u);

    /* Кнопка питания (иконка) справа. */
    s_power_w = 30u;
    s_power_h = 30u;
    s_power_x = x + START_W - s_power_w - 12u;
    s_power_y = byy + (43u - s_power_h) / 2u;
    u32 mx, my;
    uidraw_mouse_pos(&mx, &my);
    u32 pfill = (uidraw_rect_hit(s_power_x, s_power_y, s_power_w, s_power_h,
                                 mx, my) != 0)
                    ? 0x2C3A52u : 0x161D29u;
    (void)fb_fill_rect(s_power_x, s_power_y, s_power_w, s_power_h, pfill);
    /* Символ питания (FND-2f): ПОЛНОЕ кольцо с разрывом под черту сверху
     * + вертикальная черта. Раньше кольцо рисовалось только выше центра
     * (dy < 4) — символ выглядел как «∧» без низа. */
    u32 pcx = s_power_x + s_power_w / 2u;
    u32 pcy = s_power_y + s_power_h / 2u + 1u;
    for (s32 dy = -9; dy <= 9; dy++) {
        for (s32 dx = -9; dx <= 9; dx++) {
            s32 d2 = dx * dx + dy * dy;
            if (d2 < 5 * 5 || d2 > 8 * 8) {
                continue;   /* кольцо радиусом ~5..8 px (толщина 2+) */
            }
            /* Разрыв кольца там, где проходит черта (верх, по центру). */
            if (dy < -4 && dx >= -2 && dx <= 1) {
                continue;
            }
            (void)fb_fill_rect((u32)(pcx + dx), (u32)(pcy + dy), 1u, 1u,
                               UI_COL_TX);
        }
    }
    /* Черта: от верхней кромки кольца к центру символа. */
    (void)fb_fill_rect(pcx, pcy - 10u, 2u, 8u, UI_COL_TX);
}

int dsk_start_click(u32 mx, u32 my)
{
    if (g_dsk.start_open == 0) {
        return 0;
    }
    u32 x, y;
    start_rect(&x, &y);

    /* Мимо панели — закрыть. */
    if (uidraw_rect_hit(x, y, START_W, START_H, mx, my) == 0) {
        g_dsk.start_open = 0;
        return 1;
    }

    u32 grid_y = y + 10u + START_SEARCH_H + 10u;

    if (g_dsk.start_search[0] == '\0') {
        u32 results[APP_COUNT];
        u32 n = start_results(results, APP_COUNT);
        for (u32 r = 0; r < TILE_ROWS; r++) {
            for (u32 c = 0; c < TILE_COLS; c++) {
                u32 i = r * TILE_COLS + c;
                if (i >= n) {
                    break;
                }
                u32 tx = x + 14u + c * (TILE_W + 6u);
                u32 ty = grid_y + r * (TILE_H + 6u);
                if (uidraw_rect_hit(tx, ty, TILE_W, TILE_H, mx, my) != 0) {
                    dsk_app_open((u8)results[i]);
                    g_dsk.start_open = 0;
                    return 1;
                }
            }
        }
    } else {
        u32 results[APP_COUNT];
        u32 n = start_results(results, APP_COUNT);
        for (u32 i = 0; i < n && i < 8u; i++) {
            u32 ry = grid_y + i * 26u;
            if (uidraw_rect_hit(x + 12u, ry, START_W - 24u, 24u, mx, my) != 0) {
                dsk_app_open((u8)results[i]);
                g_dsk.start_open = 0;
                return 1;
            }
        }
    }

    /* Питание. */
    if (uidraw_rect_hit(s_power_x, s_power_y, s_power_w, s_power_h,
                        mx, my) != 0) {
        u32 actions[4] = { MENU_LOGOUT, MENU_REBOOT, MENU_POWEROFF,
                           MENU_AMNESIA };
        dsk_menu_open(s_power_x - MENU_W + s_power_w, s_power_y - 4u * MENU_ITEM_H - 8u,
                      actions, 4);
        return 1;
    }
    return 1;   /* внутри панели — съедаем */
}

int dsk_start_key(const struct kbd_event *ev)
{
    if (g_dsk.start_open == 0) {
        return 0;
    }
    if (ev->code == KEY_ESC) {
        g_dsk.start_open = 0;
        return 1;
    }
    if (ev->code == KEY_BSPACE) {
        u32 n = (u32)strlen(g_dsk.start_search);
        if (n > 0u) {
            g_dsk.start_search[n - 1u] = '\0';
            g_dsk.start_sel = 0;
        }
        return 1;
    }
    if (ev->code == KEY_ENTER) {
        u32 results[APP_COUNT];
        u32 n = start_results(results, APP_COUNT);
        u32 pick = (g_dsk.start_sel < (s32)n) ? (u32)g_dsk.start_sel : 0u;
        if (n > 0u) {
            dsk_app_open((u8)results[pick]);
            g_dsk.start_open = 0;
        }
        return 1;
    }
    if (ev->code == KEY_DOWN || ev->code == KEY_UP) {
        if (g_dsk.start_search[0] != '\0') {
            u32 results[APP_COUNT];
            u32 n = start_results(results, APP_COUNT);
            if (n > 0u) {
                g_dsk.start_sel += (ev->code == KEY_DOWN) ? 1 : -1;
                if (g_dsk.start_sel < 0) {
                    g_dsk.start_sel = (int)n - 1;
                }
                if (g_dsk.start_sel >= (int)n) {
                    g_dsk.start_sel = 0;
                }
            }
        }
        return 1;
    }
    if (ev->ch >= 0x20 && ev->ch <= 0x7E) {
        u32 n = (u32)strlen(g_dsk.start_search);
        if (n + 1u < sizeof(g_dsk.start_search)) {
            g_dsk.start_search[n] = ev->ch;
            g_dsk.start_search[n + 1u] = '\0';
            g_dsk.start_sel = 0;
        }
        return 1;
    }
    return 1;   /* Start открыт: остальные клавиши не уводим в shell */
}

/* ------------------------------------------------------------------ */
/* Иконки рабочего стола (стиль Win11: колонка слева, двойной клик)     */
/* ------------------------------------------------------------------ */

#define DI_X      16u
#define DI_Y0     16u
#define DI_STEP   84u
#define DI_BOX    48u

/* Иконки рабочего стола — subset приложений. */
static const u8 g_desktop_icons[] = {
    APP_CMD, APP_EXPL, APP_NOTEPAD, APP_CALC, APP_SET
};
#define DI_COUNT (sizeof(g_desktop_icons) / sizeof(g_desktop_icons[0]))

static void desktop_icons_draw(void)
{
    u32 mx, my;
    uidraw_mouse_pos(&mx, &my);
    for (u32 i = 0; i < DI_COUNT; i++) {
        u8 app = g_desktop_icons[i];
        u32 ix = DI_X;
        u32 iy = DI_Y0 + i * DI_STEP;
        int sel = (g_dsk.icon_sel == (int)i);
        if (sel != 0) {
            (void)fb_fill_rect(ix - 3u, iy - 3u, DI_BOX + 6u,
                               DI_BOX + 18u, 0x2A4464u);
        }
        uidraw_app_icon(g_app_icons[app], ix, iy, DI_BOX,
                        (uidraw_rect_hit(ix, iy, DI_BOX, DI_BOX, mx, my) != 0)
                            ? 1 : 0);
        /* Подпись с тенью (Win11: белый текст + тёмная кайма). */
        u32 ly = iy + DI_BOX + 4u;
        uidraw_text(ix + 3u + 1u, ly + 1u, g_app_names[app], 0x000000u,
                    0x000000u);
        uidraw_text(ix + 3u, ly, g_app_names[app], 0xF2F4F8u, 0x000000u);
    }
}

/* Возвращает индекс иконки под точкой или -1. */
static int desktop_icon_at(u32 mx, u32 my)
{
    for (u32 i = 0; i < DI_COUNT; i++) {
        u32 ix = DI_X;
        u32 iy = DI_Y0 + i * DI_STEP;
        if (uidraw_rect_hit(ix - 3u, iy - 3u, DI_BOX + 6u, DI_BOX + 18u,
                            mx, my) != 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* menu_exec: исполнение действий меню                                  */
/* ------------------------------------------------------------------ */

static void menu_exec(u32 action)
{
    switch (action) {
    case MENU_PIN:
    case MENU_UNPIN:
        dsk_pin_toggle(s_menu_app);
        (void)dsk_cfg_save();
        break;
    case MENU_CLOSE:
        dsk_app_close(s_menu_app);
        break;
    case MENU_REFRESH:
        /* Перечитать конфиг (обои/акцент могли поменять извне). */
        dsk_cfg_load();
        dsk_cfg_apply();
        break;
    case MENU_DISPLAY:
        dsk_app_open(APP_SET);
        dskapps_settings_page(1);
        break;
    case MENU_PERSONAL:
        dsk_app_open(APP_SET);
        dskapps_settings_page(3);
        break;
    case MENU_TERMINAL:
        dsk_app_open(APP_CMD);
        break;
    case MENU_ABOUT:
        dsk_app_open(APP_ABOUT);
        break;
    case MENU_LOGOUT:
        shell_request_logout();
        break;
    case MENU_REBOOT:
        machine_reboot();
        break;
    case MENU_POWEROFF:
        machine_poweroff();
        break;
    case MENU_AMNESIA:
        blk_amnesia((blk_amnesia_active() != 0) ? 0 : 1);
        klog(KLOG_INFO, D_TAG, "menu: amnesia -> %d", blk_amnesia_active());
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Полный кадр                                                         */
/* ------------------------------------------------------------------ */

/* Синхронизация консоли с геометрией окна cmd (сохранение контента). */
static void cmd_console_sync(void)
{
    if (g_dsk.win[APP_CMD].open == 0) {
        return;
    }
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_CMD], &cx, &cy, &cw, &ch);
    if (cw == 0u || ch == 0u) {
        return;
    }
    (void)con_resize(cx, cy, cw, ch);
}

void dsk_redraw(void)
{
    if (g_dsk.ready == 0 || g_dsk.mode != DSK_GUI) {
        return;
    }
    uidraw_cursor_erase();
    fb_clip_screen();
    uidraw_background();

    desktop_icons_draw();

    /* Окна: нефокусные, затем фокусное. Консоль терминала — ЧАСТЬ окна
     * cmd: рисуется на его z-позиции. Контент ВСЕГДА под clip'ом
     * клиентской области (fb_clip_set) — гарантия границ. */
    for (u32 pass = 0; pass < 2u; pass++) {
        for (u32 i = 0; i < APP_COUNT; i++) {
            if (g_dsk.win[i].open == 0) {
                continue;
            }
            int is_focus = (i == g_dsk.focus);
            if ((pass == 0u) == (is_focus != 0)) {
                continue;
            }
            dsk_draw_frame((u8)i);

            u32 cx, cy, cw, ch;
            dsk_win_content(&g_dsk.win[i], &cx, &cy, &cw, &ch);
            if (i == APP_CMD) {
                /* FND-2f: привязка консоли к окну НА КАЖДОМ кадре —
                 * сетка/origin не могут разойтись с окном (следы
                 * "раздвоенной консоли" вне окна невозможны по построению). */
                cmd_console_sync();
                (void)fb_fill_rect(cx, cy, cw, ch, con_bg_rgb());
                fb_clip_set(cx, cy, cw, ch);
                /* Console обязана перерисоваться КАЖДЫЙ кадр: заливка выше
                 * стёрла предыдущий вывод, а con_flush скипает по своему
                 * dirty-флагу (текст «пропадал» при следующем событии мыши). */
                con_mark_dirty();
                con_flush();
                fb_clip_screen();
            } else {
                fb_clip_set(cx, cy, cw, ch);
                dsk_app_draw((u8)i);
                fb_clip_screen();
            }
        }
    }

    dsk_taskbar_draw();
    dsk_start_draw();
    dsk_menu_draw();
    uidraw_cursor_draw();
    fb_present();   /* готовый кадр целиком -> VRAM (без мерцания) */

    g_dsk.need_redraw = 0;
}

void dsk_request_redraw(void)
{
    if (g_dsk.ready != 0) {
        g_dsk.need_redraw = 1;
    }
}

int dsk_redraw_pending(void)
{
    return (g_dsk.ready != 0 && g_dsk.mode == DSK_GUI) ? g_dsk.need_redraw : 0;
}

int dsk_cmd_open(void)
{
    return (g_dsk.ready != 0) ? g_dsk.win[APP_CMD].open : 0;
}

/* ------------------------------------------------------------------ */
/* Мышь: перенос/растягивание/клики                                    */
/* ------------------------------------------------------------------ */

/* Ищет окно, которому принадлежит точка (фокусное сверху). */
static int window_at(u32 mx, u32 my)
{
    if (g_dsk.win[g_dsk.focus].open != 0) {
        const struct dsk_win *w = &g_dsk.win[g_dsk.focus];
        if (uidraw_rect_hit(w->x, w->y, w->w, w->h, mx, my) != 0) {
            return (int)g_dsk.focus;
        }
    }
    for (u32 i = 0; i < APP_COUNT; i++) {
        if (i == g_dsk.focus || g_dsk.win[i].open == 0) {
            continue;
        }
        const struct dsk_win *w = &g_dsk.win[i];
        if (uidraw_rect_hit(w->x, w->y, w->w, w->h, mx, my) != 0) {
            return (int)i;
        }
    }
    return -1;
}

static void clamp_win(struct dsk_win *w)
{
    if (w->w < WIN_MIN_W) { w->w = WIN_MIN_W; }
    if (w->h < WIN_MIN_H) { w->h = WIN_MIN_H; }
    if (w->x + w->w > g_dsk.w) {
        w->x = (g_dsk.w > w->w) ? g_dsk.w - w->w : 0u;
    }
    if (w->y + w->h > g_dsk.h - TB_H) {
        w->y = (g_dsk.h - TB_H > w->h) ? g_dsk.h - TB_H - w->h : 0u;
    }
    if ((s32)w->x < 0) { w->x = 0; }
    if ((s32)w->y < 0) { w->y = 0; }
}

static void handle_left_click(u32 mx, u32 my)
{
    /* 1. Меню — самый верхний слой. */
    if (g_dsk.menu_open != 0) {
        (void)dsk_menu_click(mx, my);
        return;
    }
    /* 2. Start-меню. */
    if (g_dsk.start_open != 0) {
        (void)dsk_start_click(mx, my);
        return;
    }
    /* 3. Окна по z-порядку. */
    int hit = window_at(mx, my);
    if (hit >= 0) {
        struct dsk_win *w = &g_dsk.win[hit];
        g_dsk.focus = (u8)hit;

        u32 edge = dsk_edge_at(w, mx, my);
        if (edge != 0) {
            g_dsk.drag_mode = 2;
            g_dsk.drag_win = (u8)hit;
            g_dsk.drag_mask = edge;
            g_dsk.drag_mx = mx;
            g_dsk.drag_my = my;
            g_dsk.drag_orig = *w;
            return;
        }

        /* Кнопка закрытия. */
        u32 cx = w->x + w->w - 16u;
        u32 cy = w->y + (TITLE_H - 12u) / 2u + 1u;
        if (uidraw_rect_hit(cx, cy, 12u, 12u, mx, my) != 0) {
            dsk_app_close((u8)hit);
            return;
        }

        /* Заголовок -> перенос. */
        if (my >= w->y && my < w->y + TITLE_H) {
            g_dsk.drag_mode = 1;
            g_dsk.drag_win = (u8)hit;
            g_dsk.drag_mx = mx;
            g_dsk.drag_my = my;
            g_dsk.drag_orig = *w;
            return;
        }

        /* Контент приложения. */
        (void)dsk_app_click((u8)hit, mx, my);
        return;
    }

    /* 4. Панель задач. */
    if (dsk_taskbar_click(mx, my, 0) != 0) {
        return;
    }

    /* 5. Иконки рабочего стола (двойной клик = открытие). */
    int di = desktop_icon_at(mx, my);
    if (di >= 0) {
        u32 now = timer_uptime_ms();
        if (g_dsk.last_click_icon == di &&
            now - g_dsk.last_click_ms <= 500u) {
            dsk_app_open(g_desktop_icons[di]);
            g_dsk.last_click_icon = -1;
        } else {
            g_dsk.icon_sel = di;
            g_dsk.last_click_icon = di;
            g_dsk.last_click_ms = now;
        }
        return;
    }
    g_dsk.icon_sel = -1;
}

static void handle_right_click(u32 mx, u32 my)
{
    if (g_dsk.menu_open != 0 || g_dsk.start_open != 0) {
        dsk_menu_close();
        g_dsk.start_open = 0;
        return;
    }

    int hit = window_at(mx, my);
    if (hit >= 0) {
        g_dsk.focus = (u8)hit;
        s_menu_app = (u8)hit;
        u32 actions[3];
        u32 count = 0;
        actions[count++] = MENU_CLOSE;
        actions[count++] = dsk_pin_index((u8)hit) >= 0 ? MENU_UNPIN : MENU_PIN;
        dsk_menu_open(mx, my, actions, count);
        return;
    }
    if (dsk_taskbar_click(mx, my, 1) != 0) {
        return;
    }

    /* Меню рабочего стола. */
    u32 actions[5] = { MENU_REFRESH, MENU_TERMINAL, MENU_DISPLAY,
                       MENU_PERSONAL, MENU_ABOUT };
    dsk_menu_open(mx, my, actions, 5);
}

void dsk_mouse_event(const struct mouse_event *ev)
{
    if (g_dsk.ready == 0 || g_dsk.mode != DSK_GUI) {
        ui_mouse_event(ev);
        return;
    }

    static u8 prev_buttons;
    uidraw_mouse_event(ev);
    u32 mx, my;
    uidraw_mouse_pos(&mx, &my);

    if (g_dsk.drag_mode != 0 && (ev->buttons & MOUSE_BTN_LEFT) != 0u) {
        s32 dmx = (s32)mx - (s32)g_dsk.drag_mx;
        s32 dmy = (s32)my - (s32)g_dsk.drag_my;
        struct dsk_win *w = &g_dsk.win[g_dsk.drag_win];
        if (g_dsk.drag_mode == 1) {
            w->x = (u32)((s32)g_dsk.drag_orig.x + dmx);
            w->y = (u32)((s32)g_dsk.drag_orig.y + dmy);
            clamp_win(w);
        } else {
            /* Resize по маске краёв. */
            u32 m = g_dsk.drag_mask;
            struct dsk_win *o = &g_dsk.drag_orig;
            if ((m & EDGE_R) != 0u) {
                w->w = (u32)((s32)o->w + dmx);
            }
            if ((m & EDGE_B) != 0u) {
                w->h = (u32)((s32)o->h + dmy);
            }
            if ((m & EDGE_L) != 0u) {
                s32 nx = (s32)o->x + dmx;
                s32 nw = (s32)o->w - dmx;
                if (nw < (s32)WIN_MIN_W) {
                    nx = (s32)(o->x + o->w - WIN_MIN_W);
                    nw = (s32)WIN_MIN_W;
                }
                w->x = (u32)nx;
                w->w = (u32)nw;
            }
            if ((m & EDGE_T) != 0u) {
                s32 ny = (s32)o->y + dmy;
                s32 nh = (s32)o->h - dmy;
                if (nh < (s32)WIN_MIN_H) {
                    ny = (s32)(o->y + o->h - WIN_MIN_H);
                    nh = (s32)WIN_MIN_H;
                }
                w->y = (u32)ny;
                w->h = (u32)nh;
            }
            clamp_win(w);
        }
        cmd_console_sync();
    } else if (g_dsk.drag_mode != 0) {
        g_dsk.drag_mode = 0;   /* кнопку отпустили */
    }

    if ((ev->buttons & MOUSE_BTN_LEFT) != 0u &&
        (prev_buttons & MOUSE_BTN_LEFT) == 0u) {
        handle_left_click(mx, my);
    }
    if ((ev->buttons & MOUSE_BTN_RIGHT) != 0u &&
        (prev_buttons & MOUSE_BTN_RIGHT) == 0u) {
        handle_right_click(mx, my);
    }
    prev_buttons = ev->buttons;

    dsk_request_redraw();
}

/* ------------------------------------------------------------------ */
/* Клавиатура: Start -> текстовые приложения -> shell                  */
/* ------------------------------------------------------------------ */

int dsk_kbd_event(const struct kbd_event *ev)
{
    if (g_dsk.ready == 0 || g_dsk.mode != DSK_GUI) {
        return 0;
    }
    if (g_dsk.menu_open != 0) {
        if (ev->code == KEY_ESC) {
            dsk_menu_close();
            return 1;
        }
        return 1;   /* меню открыто — ввод не уводим в shell */
    }
    if (g_dsk.start_open != 0) {
        return dsk_start_key(ev);
    }
    if (g_dsk.win[g_dsk.focus].open != 0 && g_dsk.focus != APP_CMD) {
        return dsk_app_key(g_dsk.focus, ev);
    }
    return 0;   /* shell (терминал) */
}

/* ------------------------------------------------------------------ */
/* Конфигурация \fobos\fobos.cfg (v2) и закрепления                    */
/* ------------------------------------------------------------------ */

#define FOBOS_CFG_PATH "\\fobos\\fobos.cfg"

/* Парс "123" -> u32 (без знака; 0 при ошибке, но и "0" даёт 0 —
 * для наших ключей это приемлемо: id обоев 0 валиден, остальные
 * валидируются диапазоном). Возвращает 1 при успехе. */
static int parse_u32_ok(const char *s, u32 *out)
{
    u32 v = 0;
    int any = 0;
    for (u32 i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        if (v > 0xFFFFFFFFu / 10u) {
            return 0;
        }
        v = v * 10u + (u32)(s[i] - '0');
        any = 1;
    }
    if (any == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

static u32 parse_u32(const char *s)
{
    u32 v = 0;
    (void)parse_u32_ok(s, &v);
    return v;
}

/* Парс "1280x800" -> (1280, 800). 0 = успех. */
static int parse_wxh(const char *s, u32 *out_w, u32 *out_h)
{
    u32 i = 0;
    u32 w = 0, h = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        w = w * 10u + (u32)(s[i] - '0');
        i++;
    }
    if (i == 0u || s[i] != 'x') {
        return -1;
    }
    i++;
    while (s[i] >= '0' && s[i] <= '9') {
        h = h * 10u + (u32)(s[i] - '0');
        i++;
    }
    if (s[i] != '\0' || w == 0u || h == 0u) {
        return -1;
    }
    *out_w = w;
    *out_h = h;
    return 0;
}

void dsk_pins_default(void)
{
    static const char *const def[] = { "cmd", "explorer", "notepad",
                                       "calc", "settings" };
    g_dsk.cfg_pin_count = 0;
    for (u32 i = 0; i < sizeof(def) / sizeof(def[0]); i++) {
        for (u32 k = 0; def[i][k] != '\0' && k + 1u < APP_NAME_MAX; k++) {
            g_dsk.cfg_pins[i][k] = def[i][k];
        }
        g_dsk.cfg_pin_count++;
    }
}

int dsk_pin_index(u8 app)
{
    for (u32 p = 0; p < g_dsk.cfg_pin_count; p++) {
        if (strcmp(g_dsk.cfg_pins[p], g_app_names[app]) == 0) {
            return (int)p;
        }
    }
    return -1;
}

void dsk_pin_toggle(u8 app)
{
    int at = dsk_pin_index(app);
    if (at >= 0) {
        for (u32 p = (u32)at; p + 1u < g_dsk.cfg_pin_count; p++) {
            for (u32 k = 0; k < APP_NAME_MAX; k++) {
                g_dsk.cfg_pins[p][k] = g_dsk.cfg_pins[p + 1u][k];
            }
        }
        g_dsk.cfg_pin_count--;
        return;
    }
    if (g_dsk.cfg_pin_count < APP_COUNT) {
        for (u32 k = 0; g_app_names[app][k] != '\0' && k + 1u < APP_NAME_MAX;
             k++) {
            g_dsk.cfg_pins[g_dsk.cfg_pin_count][k] = g_app_names[app][k];
        }
        g_dsk.cfg_pin_count++;
    }
}

void dsk_cfg_apply(void)
{
    uidraw_set_accent(g_dsk.cfg_accent);
    uidraw_set_wall(g_dsk.cfg_wall);
}

void dsk_cfg_load(void)
{
    /* Дефолты (файл может отсутствовать — это норма). */
    g_dsk.cfg_res_w = 0;
    g_dsk.cfg_res_h = 0;
    g_dsk.cfg_wall = UI_WALL_BLOOM;
    g_dsk.cfg_accent = UI_COL_ACCENT;
    g_dsk.cfg_time24 = 1;
    dsk_pins_default();
    if (s_locked_mode < 0) {
        s_locked_mode = -1;
    }

    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return;
    }
    u8 buf[512];
    u32 len = 0;
    if (fat_file_read(v, FOBOS_CFG_PATH, buf, sizeof(buf) - 1u, &len)
        != K_OK) {
        return;
    }
    buf[len] = 0;

    struct cfglite_kv kvs[12];
    u32 n = cfglite_parse((const char *)buf, len, kvs, 12);

    const char *s = cfglite_get(kvs, n, "mode");
    if (s != NULL) {
        if (strcmp(s, "gui") == 0) {
            s_locked_mode = DSK_GUI;
        } else if (strcmp(s, "tui") == 0) {
            s_locked_mode = DSK_TUI;
        }
    }
    s = cfglite_get(kvs, n, "res");
    if (s != NULL) {
        u32 rw = 0, rh = 0;
        if (parse_wxh(s, &rw, &rh) == 0 && rw >= 320u && rh >= 240u) {
            g_dsk.cfg_res_w = rw;
            g_dsk.cfg_res_h = rh;
        }
    }
    s = cfglite_get(kvs, n, "wall");
    if (s != NULL) {
        u32 id = parse_u32(s);
        if (id < UI_WALL_COUNT) {
            g_dsk.cfg_wall = id;
        }
    }
    s = cfglite_get(kvs, n, "accent");
    if (s != NULL && strlen(s) == 6u) {
        u32 rgb = 0;
        int ok = 1;
        for (u32 i = 0; i < 6u; i++) {
            char c = s[i];
            u32 d;
            if (c >= '0' && c <= '9')      { d = (u32)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { d = (u32)(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { d = (u32)(c - 'A' + 10); }
            else { ok = 0; break; }
            rgb = rgb * 16u + d;
        }
        if (ok != 0) {
            g_dsk.cfg_accent = rgb;
        }
    }
    s = cfglite_get(kvs, n, "time24");
    if (s != NULL) {
        g_dsk.cfg_time24 = (strcmp(s, "0") == 0) ? 0 : 1;
    }
    s = cfglite_get(kvs, n, "pins");
    if (s != NULL) {
        /* Разбор "cmd, explorer, notepad". */
        g_dsk.cfg_pin_count = 0;
        u32 i = 0;
        while (s[i] != '\0' && g_dsk.cfg_pin_count < APP_COUNT) {
            while (s[i] == ' ' || s[i] == ',') { i++; }
            u32 k = 0;
            while (s[i] != '\0' && s[i] != ',' && k + 1u < APP_NAME_MAX) {
                g_dsk.cfg_pins[g_dsk.cfg_pin_count][k++] = s[i++];
            }
            g_dsk.cfg_pins[g_dsk.cfg_pin_count][k] = '\0';
            if (k > 0u) {
                g_dsk.cfg_pin_count++;
            }
        }
    }
}

err_t dsk_cfg_save(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return K_ENODEV;
    }
    char buf[320];
    u32 o = 0;
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o,
                        "# fobos.cfg (FND-2e)\n");
    if (s_locked_mode >= 0) {
        o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "mode=%s\n",
                            (s_locked_mode == DSK_GUI) ? "gui" : "tui");
    }
    if (g_dsk.cfg_res_w != 0u && g_dsk.cfg_res_h != 0u) {
        o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "res=%ux%u\n",
                            g_dsk.cfg_res_w, g_dsk.cfg_res_h);
    }
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "wall=%u\n", g_dsk.cfg_wall);
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "accent=%06x\n",
                        g_dsk.cfg_accent);
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "time24=%d\n",
                        g_dsk.cfg_time24);
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "pins=");
    for (u32 p = 0; p < g_dsk.cfg_pin_count; p++) {
        o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "%s%s",
                            (p != 0) ? "," : "", g_dsk.cfg_pins[p]);
    }
    o += (u32)ksnprintf(buf + o, sizeof(buf) - o, "\n");
    if (o >= sizeof(buf)) {
        return K_ERANGE;
    }
    err_t rc = fat_file_write(v, FOBOS_CFG_PATH, (const u8 *)buf, o);
    if (rc == K_OK) {
        klog(KLOG_INFO, D_TAG, "cfg: сохранён (wall=%u accent=%06x pins=%u)",
             g_dsk.cfg_wall, g_dsk.cfg_accent, g_dsk.cfg_pin_count);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Режимы и публичный API                                              */
/* ------------------------------------------------------------------ */

int dsk_mode(void)
{
    return g_dsk.ready ? g_dsk.mode : DSK_TUI;
}

int dsk_gui_active(void)
{
    return (g_dsk.ready != 0 && g_dsk.mode == DSK_GUI) ? 1 : 0;
}

err_t dsk_init(int mode)
{
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    g_dsk.w = w;
    g_dsk.h = h;
    g_dsk.last_second = 0xFFFFFFFFu;

    for (u32 i = 0; i < APP_COUNT; i++) {
        g_dsk.win[i].open = 0;
        dsk_win_default((u8)i, &g_dsk.win[i]);
    }
    g_dsk.focus = APP_CMD;
    g_dsk.drag_mode = 0;
    g_dsk.icon_sel = -1;
    g_dsk.last_click_icon = -1;
    g_dsk.last_click_ms = 0;
    g_dsk.menu_open = 0;
    g_dsk.menu_count = 0;
    g_dsk.start_open = 0;
    g_dsk.start_search[0] = '\0';
    g_dsk.start_sel = 0;

    /* FND-2f: новая сессия — чистая консоль. Без этого устаревшая сетка/
     * контент прежней сессии смешивались с новой (баннер не в том месте,
     * чужие строки в окне cmd). */
    ui_console_session_reset();

    /* Конфигурация (обои/акцент/пины/разрешение) из fobos.cfg. */
    dsk_cfg_load();
    dsk_cfg_apply();

    dskapps_reset();
    g_dsk.ready = 1;

    return dsk_switch(mode);
}

err_t dsk_switch(int mode)
{
    if (g_dsk.ready == 0) {
        return K_ESTATE;
    }
    if (mode != DSK_TUI && mode != DSK_GUI) {
        return K_EINVAL;
    }
    g_dsk.mode = mode;
    dsk_menu_close();
    g_dsk.start_open = 0;

    if (mode == DSK_TUI) {
        /* Консольная панель FND-2b. Курсор — кадровый слой: события мыши
         * меняют координаты, спрайт печатает конвейер TUI (main_loop). */
        g_dsk.win[APP_CMD].open = 0;
        uidraw_cursor_defer(1);
        err_t rc = ui_init();
        if (rc != K_OK) {
            return rc;
        }
    } else {
        /* GUI: окно cmd открыто по умолчанию, консоль в его области.
         * Курсор рисуется в скобках erase->draw (полный кадр). */
        uidraw_cursor_defer(0);
        if (g_dsk.win[APP_CMD].open == 0) {
            g_dsk.win[APP_CMD].open = 1;
            dsk_win_default(APP_CMD, &g_dsk.win[APP_CMD]);
        }
        cmd_console_sync();
        dsk_redraw();
    }
    shell_reset_line();
    klog(KLOG_INFO, D_TAG, "desktop: %s mode active",
         (mode == DSK_GUI) ? "gui" : "tui");
    return K_OK;
}

err_t dsk_lock_mode(int mode)
{
    if (mode != DSK_TUI && mode != DSK_GUI) {
        return K_EINVAL;
    }
    s_locked_mode = mode;
    return dsk_cfg_save();
}

err_t dsk_read_lock(int *out_mode)
{
    if (out_mode == NULL) {
        return K_EINVAL;
    }
    /* Свежее чтение файла (kmain вызывает до dsk_init). */
    s_locked_mode = -1;
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return K_ENODEV;
    }
    u8 buf[512];
    u32 len = 0;
    err_t rc = fat_file_read(v, FOBOS_CFG_PATH, buf, sizeof(buf) - 1u, &len);
    if (rc != K_OK) {
        return rc;
    }
    buf[len] = 0;

    struct cfglite_kv kvs[12];
    u32 n = cfglite_parse((const char *)buf, len, kvs, 12);
    const char *s = cfglite_get(kvs, n, "mode");
    if (s == NULL) {
        return K_ENOENT;
    }
    if (strcmp(s, "gui") == 0) {
        s_locked_mode = DSK_GUI;
        *out_mode = DSK_GUI;
        return K_OK;
    }
    if (strcmp(s, "tui") == 0) {
        s_locked_mode = DSK_TUI;
        *out_mode = DSK_TUI;
        return K_OK;
    }
    return K_EINVAL;
}

int dsk_statusbar_due(void)
{
    if (g_dsk.ready == 0) {
        return 0;
    }
    u32 sec = (u32)(timer_uptime_ms() / 1000ull);
    if (sec != g_dsk.last_second) {
        g_dsk.last_second = sec;
        return 1;
    }
    return 0;
}

void dsk_statusbar_update(void)
{
    if (g_dsk.ready == 0) {
        return;
    }
    if (g_dsk.mode == DSK_TUI) {
        ui_statusbar_update();
    } else {
        /* Часы на панели: перерисовываем только панель + present.
         * Скобки erase->draw обязательны: панель перекрашивает зону
         * спрайта, если курсор стоит над таскбаром. */
        uidraw_cursor_erase();
        dsk_taskbar_draw();
        uidraw_cursor_draw();
        fb_present();
    }
}

/* --- Приложения: открытие/перечисление --- */

void dsk_start_toggle(void);   /* определён выше */

err_t dsk_open_app_by_name(const char *name)
{
    if (name == NULL) {
        return K_EINVAL;
    }
    for (u32 i = 0; i < APP_COUNT; i++) {
        if (strcmp(g_app_names[i], name) == 0) {
            if (g_dsk.ready == 0) {
                return K_ESTATE;
            }
            dsk_app_open((u8)i);
            dsk_request_redraw();
            return K_OK;
        }
    }
    return K_ENOENT;
}

int dsk_app_count(void)
{
    return (int)APP_COUNT;
}

const char *dsk_app_name(int idx)
{
    if (idx < 0 || (u32)idx >= APP_COUNT) {
        return NULL;
    }
    return g_app_names[idx];
}

int dsk_app_is_open(int idx)
{
    if (idx < 0 || (u32)idx >= APP_COUNT || g_dsk.ready == 0) {
        return 0;
    }
    return (g_dsk.win[idx].open != 0) ? 1 : 0;
}

int dsk_app_is_pinned(int idx)
{
    if (idx < 0 || (u32)idx >= APP_COUNT) {
        return 0;
    }
    return (dsk_pin_index((u8)idx) >= 0) ? 1 : 0;
}
