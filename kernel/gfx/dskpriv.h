/*
 * Fobos OS — внутреннее состояние рабочего стола (FND-2e).
 *
 * ОБЩИЙ заголовок desktop.c (оконный менеджер: компоновка, z-order,
 * resize, таскбар, Start-меню, меню) и dskapps.c (содержимое окон:
 * explorer/viewer/about/notepad/calc/settings). НЕ включается наружу —
 * внешний API только desktop.h.
 */
#ifndef FOBOS_GFX_DSKPRIV_H
#define FOBOS_GFX_DSKPRIV_H

#include <fobos/types.h>
#include <fobos/errors.h>

#include "desktop.h"

/* Идентификаторы приложений == индексы окон (z-модель плоская: одно
 * фокусное окно сверху, остальные в фиксированном порядке). */
enum {
    APP_CMD = 0,      /* терминал (kshell в окне)            */
    APP_EXPL,         /* explorer — файлы FAT32              */
    APP_ABOUT,        /* о системе                           */
    APP_VIEW,         /* просмотр текстового файла           */
    APP_NOTEPAD,      /* редактор                            */
    APP_CALC,         /* калькулятор                         */
    APP_SET,          /* настройки                           */
    APP_COUNT
};

/* Имена приложений: иконки рабочего стола/пины/команды shell — один
 * словарь. Порядок совпадает с enum. */
#define APP_NAME_MAX 12
extern const char *const g_app_names[APP_COUNT];

/* --- Геометрия --- */
#define TB_H       36u   /* высота панели задач */
#define TITLE_H    20u   /* высота заголовка окна */
#define EDGE_PX     4u   /* ширина зоны resize у края окна */
#define WIN_MIN_W 180u
#define WIN_MIN_H 140u

/* Битовая маска краёв для resize (edge_at). */
#define EDGE_R 0x1u
#define EDGE_B 0x2u
#define EDGE_L 0x4u
#define EDGE_T 0x8u

struct dsk_win {
    int open;
    u32 x, y, w, h;
    int resizable;
};

/* --- Состояние рабочего стола (одно на систему; рисует только kmain) --- */
struct dsk_state {
    int ready;
    int mode;                 /* DSK_TUI / DSK_GUI */
    u32 w, h;                 /* размеры экрана */

    struct dsk_win win[APP_COUNT];
    u8 focus;                 /* индекс фокусного окна */

    /* Перетаскивание/растягивание. */
    int drag_mode;            /* 0 = нет, 1 = перенос, 2 = resize */
    u8 drag_win;
    u32 drag_mask;            /* EDGE_* для resize */
    u32 drag_mx, drag_my;     /* стартовая позиция мыши */
    struct dsk_win drag_orig; /* стартовая геометрия окна */

    /* Рабочий стол: выбор иконки, двойной клик. */
    int icon_sel;             /* -1 = нет */
    u32 last_click_ms;
    int last_click_icon;

    /* Часы/коалесинг. */
    u32 last_second;
    int need_redraw;

    /* Конфигурация (\fobos\fobos.cfg v2). */
    u32 cfg_res_w, cfg_res_h; /* разрешение после перезагрузки (0 = авто) */
    u32 cfg_wall;             /* UI_WALL_* */
    u32 cfg_accent;           /* 0xRRGGBB */
    int cfg_time24;           /* формат часов на панели */
    char cfg_pins[APP_COUNT][APP_NAME_MAX];
    u32 cfg_pin_count;

    /* Всплывающее меню (одно на экран). */
    int menu_open;
    u32 menu_x, menu_y;
    u32 menu_count;
    u32 menu_action[8];

    /* Start-меню. */
    int start_open;
    char start_search[24];
    int start_sel;
};

extern struct dsk_state g_dsk;

/* --- Геометрия окон (desktop.c) --- */
void dsk_win_default(u8 app, struct dsk_win *w);
void dsk_win_content(const struct dsk_win *w, u32 *cx, u32 *cy,
                     u32 *cw, u32 *ch);
u32  dsk_edge_at(const struct dsk_win *w, u32 mx, u32 my);
void dsk_draw_frame(u8 app);

/* --- Действия меню (код action из enum menu_action) --- */
enum menu_action {
    MENU_NONE = 0,
    MENU_PIN, MENU_UNPIN, MENU_CLOSE,
    MENU_REFRESH, MENU_DISPLAY, MENU_PERSONAL, MENU_TERMINAL, MENU_ABOUT,
    MENU_LOGOUT, MENU_REBOOT, MENU_POWEROFF, MENU_AMNESIA
};
void dsk_menu_open(u32 x, u32 y, const u32 *actions, u32 count);
void dsk_menu_close(void);
void dsk_menu_draw(void);
/* 1 = клик обработан меню (в т.ч. «мимо меню» — закрыть). */
int  dsk_menu_click(u32 mx, u32 my);

/* --- Приложения (dskapps.c) --- */
void dskapps_reset(void);
void dsk_app_draw(u8 app);
int  dsk_app_click(u8 app, u32 mx, u32 my);   /* 1 = обработано */
int  dsk_app_key(u8 app, const struct kbd_event *ev); /* 1 = обработано */

/* Заголовок окна viewer (имя файла) для рамки. */
void dskapps_view_title(char *out, u32 cap);

/* Открыть Settings на конкретной странице (0 System, 1 Display,
 * 2 Time, 3 Personal) — из контекстного меню рабочего стола. */
void dskapps_settings_page(u32 page);

/* Открытие окна приложения (win_default + фокус + side-эффекты). */
void dsk_app_open(u8 app);
void dsk_app_close(u8 app);

/* --- Панель задач и Start (desktop.c) --- */
void dsk_taskbar_draw(void);
int  dsk_taskbar_click(u32 mx, u32 my, int right_click); /* 1 = обработано */
void dsk_start_toggle(void);
int  dsk_start_is_open(void);
void dsk_start_draw(void);
int  dsk_start_click(u32 mx, u32 my);   /* 1 = обработано */
int  dsk_start_key(const struct kbd_event *ev); /* 1 = обработано */

/* --- Конфигурация fobos.cfg v2 (desktop.c) --- */
void dsk_cfg_load(void);
err_t dsk_cfg_save(void);      /* пишет mode/res/wall/accent/time24/pins */
void dsk_cfg_apply(void);      /* accent/wall -> uidraw */
void dsk_pins_default(void);
int  dsk_pin_index(u8 app);    /* позиция app в pins или -1 */
void dsk_pin_toggle(u8 app);

#endif /* FOBOS_GFX_DSKPRIV_H */
