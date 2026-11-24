/*
 * Fobos OS — рабочий стол с двумя режимами (этап FND-2e).
 *
 * DSK_TUI — консольная панель FND-2b (ui.c) как единственная область.
 * DSK_GUI — графический рабочий стол в стиле Win11:
 *   - процедурные обои (4 варианта), иконки рабочего стола (двойной
 *     клик открывает), контекстное меню по правой кнопке;
 *   - окна приложений: перенос за заголовок, РАСТЯГИВАНИЕ за 8 зон
 *     (края и углы), фокус/z-order, кнопка закрытия;
 *   - панель задач: кнопка Пуск с логотипом, ЦЕНТРИРОВАННАЯ группа
 *     ТОЛЬКО ИЗ ИКОНОК (закрепления — pins, сохраняются в fobos.cfg),
 *     pill-индикаторы запущенных, справа AMNESIA и часы/дата;
 *   - полноценное Пуск-меню: поиск, плитки приложений, пользователь,
 *     питание (logout/reboot/poweroff/амнезия);
 *   - клиппинг всего содержимого окон на уровне рендера (fb_clip).
 *
 * Приложения: cmd / explorer / about / viewer / notepad / calc /
 * settings (см. dskapps.c).
 *
 * Переключение режимов — команда kshell `fos == gui|tui [--lock]`.
 */
#ifndef FOBOS_GFX_DESKTOP_H
#define FOBOS_GFX_DESKTOP_H

#include <fobos/errors.h>
#include <fobos/types.h>

#include "../drivers/kbd.h"

struct mouse_event;

#define DSK_TUI 0
#define DSK_GUI 1

/* Инициализация и первый кадр (mode = DSK_TUI/DSK_GUI). */
err_t dsk_init(int mode);

int   dsk_mode(void);
int   dsk_gui_active(void);

/* Переключение режима с перерисовкой. Консоль перепривязывается
 * (содержимое очищается — задокументированное поведение). */
err_t dsk_switch(int mode);

/* Сохранение режима в \fobos\fobos.cfg (требует смонтированный FAT32). */
err_t dsk_lock_mode(int mode);

/* Чтение залоченного режима из \fobos\fobos.cfg.
 * out_mode: 0 = tui, 1 = gui. K_ENOENT — файла нет. */
err_t dsk_read_lock(int *out_mode);

void  dsk_mouse_event(const struct mouse_event *ev);

/* Маршрутизация клавиатуры (FND-2e): Start-меню и текстовые приложения
 * (notepad/calc) перехватывают ввод; 1 = событие поглощено, 0 — отдать
 * kshell (терминал). Вызывается из main_loop ДО shell_input. */
int   dsk_kbd_event(const struct kbd_event *ev);

int   dsk_statusbar_due(void);
void  dsk_statusbar_update(void);

/* Полная перерисовка GUI-режима (обои/иконки/окна/панель/Start) +
 * fb_present(). Консоль терминала рисуется НА z-позиции окна cmd. */
void  dsk_redraw(void);

/* Коалесинг кадров: события ввода ставят флаг, главный цикл собирает
 * ОДИН кадр на пачку событий. */
void  dsk_request_redraw(void);
int   dsk_redraw_pending(void);

/* Открыто ли окно терминала (cmd) — главный цикл учитывает при решении
 * «dirty-консоль = собрать кадр». */
int   dsk_cmd_open(void);

/* --- Приложения (FND-2e) --- */

/* Открыть приложение по имени ("cmd","explorer","about","viewer",
 * "notepad","calc","settings"). K_ENOENT — неизвестное имя. */
err_t dsk_open_app_by_name(const char *name);

/* Пуск-меню: показать/скрыть. */
void  dsk_start_toggle(void);

/* Перечисление приложений для команды `apps`/`pins` (idx = 0..). */
int         dsk_app_count(void);
const char *dsk_app_name(int idx);
int         dsk_app_is_open(int idx);
int         dsk_app_is_pinned(int idx);

#endif /* FOBOS_GFX_DESKTOP_H */
