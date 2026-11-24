/*
 * Fobos OS — реализация экрана входа (см. login.h).
 */
#include "login.h"

#include "uidraw.h"
#include "../version.h"
#include "../drivers/fb.h"
#include "../drivers/rtc.h"
#include "../drivers/kbd.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../sys/users.h"
#include "../sys/power.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"

static const char *const LG_TAG = "gfx/login";

#define CARD_W        380u
#define CARD_H        152u
#define FIELD_H       16u
#define NAME_MAX_INP  31u
#define PW_MAX_INP    63u

/* Геометрия карточки (вычисляется от размеров экрана). */
struct login_geom {
    u32 cx, cy;            /* левый верх карточки */
    u32 name_y, pw_y;      /* поля */
    u32 btn_y;
    u32 btn_login_x, btn_reboot_x, btn_shutdown_x;
    u32 btn_w;
};

struct login_state {
    char name[NAME_MAX_INP + 2u];
    char pw[PW_MAX_INP + 2u];
    u32 name_len, pw_len;
    u32 name_pos, pw_pos;
    int focus;             /* 0 = имя, 1 = пароль, 2..4 = кнопки */
    char message[64];
    u32 message_col;
};

static void geom_compute(struct login_geom *g)
{
    u32 w = uidraw_width();
    u32 h = uidraw_height();
    g->cx = (w > CARD_W) ? (w - CARD_W) / 2u : 0u;
    g->cy = (h > CARD_H + 80u) ? (h - CARD_H) / 2u : 40u;
    g->name_y = g->cy + 56u;
    g->pw_y = g->name_y + FIELD_H + 10u;
    g->btn_y = g->pw_y + FIELD_H + 22u;
    g->btn_w = (CARD_W - 24u - 12u) / 3u;
    g->btn_login_x = g->cx + 12u;
    g->btn_reboot_x = g->btn_login_x + g->btn_w + 6u;
    g->btn_shutdown_x = g->btn_reboot_x + g->btn_w + 6u;
}

static void draw_all(const struct login_geom *g, const struct login_state *s,
                     const char *clock)
{
    u32 w = uidraw_width();
    u32 h = uidraw_height();

    uidraw_background();

    /* Верхняя полоса: продукт слева, часы справа. */
    (void)fb_fill_rect(0, 0, w, 24u, UI_COL_BAR);
    (void)fb_fill_rect(0, 24u, w, 1u, UI_COL_BORDER);
    char left[80];
    (void)ksnprintf(left, sizeof(left), " Fobos OS %s", FOBOS_VERSION_STR);
    uidraw_text(8u, 8u, left, UI_COL_TX_DIM, UI_COL_BAR);
    if (clock[0] != '\0') {
        u32 tw = (u32)strlen(clock) * UI_TEXT_W;
        uidraw_text((w > tw + 8u) ? w - tw - 8u : 0u, 8u, clock,
                    UI_COL_TX_DIM, UI_COL_BAR);
    }

    uidraw_box(g->cx, g->cy, CARD_W, CARD_H, UI_COL_PANEL, UI_COL_BORDER);

    uidraw_text_centered(g->cy + 12u, "Log in to Fobos OS", UI_COL_TX, UI_COL_PANEL);

    u32 lab_x = g->cx + 12u;
    uidraw_text(lab_x, g->name_y - 11u, "Username", UI_COL_HEADER_TX, UI_COL_PANEL);
    uidraw_input_field(lab_x, g->name_y, CARD_W - 24u, s->name,
                       s->name_pos, s->focus == 0, 0);

    uidraw_text(lab_x, g->pw_y - 11u, "Password", UI_COL_HEADER_TX, UI_COL_PANEL);
    uidraw_input_field(lab_x, g->pw_y, CARD_W - 24u, s->pw,
                       s->pw_pos, s->focus == 1, 1);

    if (s->message[0] != '\0') {
        uidraw_text_centered(g->btn_y - 14u, s->message, s->message_col, UI_COL_PANEL);
    }

    uidraw_button(g->btn_login_x, g->btn_y, g->btn_w, "Log in", s->focus == 2);
    uidraw_button(g->btn_reboot_x, g->btn_y, g->btn_w, "Reboot", s->focus == 3);
    uidraw_button(g->btn_shutdown_x, g->btn_y, g->btn_w, "Shut down", s->focus == 4);

    uidraw_text_centered(h - 24u,
                         "Enter: log in   Tab/arrows: fields   F2: reboot   F3: shut down",
                         UI_COL_HEADER_TX, UI_COL_BG_TOP);
}

static void draw_clock(char *buf, u32 cap)
{
    buf[0] = '\0';
    struct rtc_time t;
    if (rtc_ready() != 0 && rtc_read_time(&t) == K_OK) {
        char full[RTC_STR_MIN];
        if (rtc_format(full, sizeof(full), &t) >= 16) {
            /* "YYYY-MM-DD HH:MM" — первые 16 символов полной строки. */
            u32 i = 0;
            for (; i < 16u && i + 1u < cap; i++) {
                buf[i] = full[i];
            }
            buf[i] = '\0';
        }
    }
}

static void field_insert(char *buf, u32 *len, u32 *pos, u32 cap, char c)
{
    if (*len + 1u >= cap || *pos > *len) {
        return;
    }
    for (u32 i = *len; i > *pos; i--) {
        buf[i] = buf[i - 1u];
    }
    buf[*pos] = c;
    (*len)++;
    (*pos)++;
    buf[*len] = '\0';
}

static void field_backspace(char *buf, u32 *len, u32 *pos)
{
    if (*pos == 0u || *len == 0u) {
        return;
    }
    for (u32 i = *pos - 1u; i < *len - 1u; i++) {
        buf[i] = buf[i + 1u];
    }
    (*len)--;
    (*pos)--;
    buf[*len] = '\0';
}

/* Попытка входа: 1 = успех (user_idx), 0 = отказ. */
static int try_login(struct login_state *s, int *out_idx)
{
    if (s->name_len == 0u) {
        (void)ksnprintf(s->message, sizeof(s->message), "Enter username");
        s->message_col = UI_COL_ERR;
        return 0;
    }
    s->name[s->name_len] = '\0';
    s->pw[s->pw_len] = '\0';

    klog(KLOG_INFO, LG_TAG, "[login] attempt user=%s", s->name);

    if (users_verify(s->name, s->pw) != 1) {
        klog(KLOG_INFO, LG_TAG, "[login] wrong password user=%s", s->name);
        (void)ksnprintf(s->message, sizeof(s->message), "Wrong password");
        s->message_col = UI_COL_ERR;
        s->pw_len = 0;
        s->pw_pos = 0;
        s->pw[0] = '\0';
        return 0;
    }

    *out_idx = users_find(s->name);
    return (*out_idx >= 0) ? 1 : 0;
}

/* Экран «Setup required» (пользователей нет). */
static int setup_required_run(void)
{
    u32 w = uidraw_width();
    u32 h = uidraw_height();

    u32 bx = (w > 420u) ? (w - 420u) / 2u : 0u;
    u32 by = (h > 200u) ? h / 3u : 24u;

    uidraw_background();
    uidraw_box(bx, by, 420u, 96u, UI_COL_PANEL, UI_COL_BORDER);
    uidraw_text_centered(by + 12u, "Fobos OS setup is not completed", UI_COL_TX, UI_COL_PANEL);
    uidraw_text_centered(by + 32u, "Press Enter to reboot into Setup", UI_COL_HEADER_TX, UI_COL_PANEL);
    uidraw_text_centered(by + 44u, "(F3 = shut down)", UI_COL_HEADER_TX, UI_COL_PANEL);
    fb_present();   /* экран ждёт клавишу в собственном цикле */

    klog(KLOG_INFO, LG_TAG, "[boot] setup-required: waiting");

    struct kbd_event ev;
    for (;;) {
        while (kbd_poll(&ev) == K_OK) {
            if (ev.code == KEY_ENTER || ev.ch == '\n' || ev.ch == '\r') {
                return -1;
            }
            if (ev.code == KEY_F3) {
                return -2;
            }
        }
        timer_sleep_ms(10);
    }
}

int login_screen_run(void)
{
    if (users_count() == 0u) {
        return setup_required_run();
    }

    struct login_geom g;
    struct login_state s;
    geom_compute(&g);

    for (u32 i = 0; i < sizeof(s); i++) {
        ((u8 *)&s)[i] = 0;
    }

    /* Единственный пользователь — предзаполняем имя. */
    if (users_count() == 1u && users_name(0) != NULL) {
        const char *n0 = users_name(0);
        while (n0[s.name_len] != '\0' && s.name_len < NAME_MAX_INP) {
            s.name[s.name_len] = n0[s.name_len];
            s.name_len++;
        }
        s.name[s.name_len] = '\0';
        s.name_pos = s.name_len;
        s.focus = 1; /* сразу в поле пароля */
    }

    (void)ksnprintf(s.message, sizeof(s.message), "Select user and log in");
    s.message_col = UI_COL_HEADER_TX;

    klog(KLOG_INFO, LG_TAG, "[boot] login: waiting for user");

    char clock[24];
    u32 last_clock_sec = 0xFFFFFFFFu;
    u32 prev_mouse_buttons = 0;
    struct kbd_event ev;
    struct mouse_event mev;

    for (;;) {
        int need_redraw = 0;

        draw_clock(clock, sizeof(clock));
        u32 now_sec = 0;
        {
            struct rtc_time t;
            if (rtc_ready() != 0 && rtc_read_time(&t) == K_OK) {
                now_sec = (u32)t.second;
            }
        }
        if (now_sec != last_clock_sec) {
            last_clock_sec = now_sec;
            need_redraw = 1;
        }

        while (kbd_poll(&ev) == K_OK) {
            if (ev.code == KEY_ENTER || ev.ch == '\n' || ev.ch == '\r') {
                if (s.focus == 3) {
                    return -1;
                }
                if (s.focus == 4) {
                    return -2;
                }
                int idx = -1;
                if (try_login(&s, &idx) != 0) {
                    return idx;
                }
                need_redraw = 1;
                continue;
            }
            if (ev.code == KEY_F2) {
                return -1;
            }
            if (ev.code == KEY_F3) {
                return -2;
            }
            if (ev.code == KEY_TAB ||
                ev.code == KEY_DOWN || ev.code == KEY_UP ||
                (ev.ch == '\t')) {
                s.focus = (s.focus + 1u) % 5u;
                need_redraw = 1;
                continue;
            }
            if (ev.code == KEY_BSPACE || ev.ch == '\b') {
                if (s.focus == 0) {
                    field_backspace(s.name, &s.name_len, &s.name_pos);
                } else if (s.focus == 1) {
                    field_backspace(s.pw, &s.pw_len, &s.pw_pos);
                }
                need_redraw = 1;
                continue;
            }
            if (ev.ch >= 0x20 && ev.ch <= 0x7E) {
                if (s.focus == 0) {
                    field_insert(s.name, &s.name_len, &s.name_pos,
                                 NAME_MAX_INP + 1u, ev.ch);
                } else if (s.focus == 1) {
                    field_insert(s.pw, &s.pw_len, &s.pw_pos,
                                 PW_MAX_INP + 1u, ev.ch);
                }
                need_redraw = 1;
                continue;
            }
        }

        while (mouse_poll(&mev) == K_OK) {
            uidraw_mouse_event(&mev);
        }

        u32 mx, my;
        uidraw_mouse_pos(&mx, &my);
        u32 btns = uidraw_mouse_buttons();
        if (btns != prev_mouse_buttons) {
            if ((btns & MOUSE_BTN_LEFT) != 0u) {
                if (uidraw_rect_hit(g.cx + 12u, g.name_y, CARD_W - 24u, FIELD_H, mx, my)) {
                    s.focus = 0;
                    need_redraw = 1;
                } else if (uidraw_rect_hit(g.cx + 12u, g.pw_y, CARD_W - 24u, FIELD_H, mx, my)) {
                    s.focus = 1;
                    need_redraw = 1;
                } else if (uidraw_button_hit(g.btn_login_x, g.btn_y, g.btn_w, mx, my)) {
                    s.focus = 2;
                    int idx = -1;
                    if (try_login(&s, &idx) != 0) {
                        return idx;
                    }
                    need_redraw = 1;
                } else if (uidraw_button_hit(g.btn_reboot_x, g.btn_y, g.btn_w, mx, my)) {
                    return -1;
                } else if (uidraw_button_hit(g.btn_shutdown_x, g.btn_y, g.btn_w, mx, my)) {
                    return -2;
                }
            }
            prev_mouse_buttons = btns;
            need_redraw = 1;
        }

        if (need_redraw != 0) {
            uidraw_cursor_erase();
            draw_all(&g, &s, clock);
            uidraw_cursor_draw();
            fb_present();   /* экран входа рисует вне main_loop */
        }

        timer_sleep_ms(10);
    }
}
