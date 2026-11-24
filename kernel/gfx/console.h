/*
 * Fobos OS — текстовая консоль поверх framebuffer (fbcon) (прототипы).
 *
 * Ячейка = 8x8 px (шрифт font8x8), цвет — индекс в палитре 16 цветов.
 * Буфер статический (heap появится в FND-2) с запасом на 2048x1280 текстовой
 * области; прокрутка — сдвиг буфера + полная перерисовка.
 *
 * Вывод: con_write/con_printf из main-контекста (в т.ч. через klog-синк).
 * Из IRQ вызывать ЗАПРЕЩЕНО. Отрисовка — ленивая: con_flush() из главного
 * цикла только при изменениях.
 */
#ifndef FOBOS_GFX_CONSOLE_H
#define FOBOS_GFX_CONSOLE_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Индексы палитры консоли (см. con_colors в console.c). */
enum {
    CON_BLACK = 0, CON_RED, CON_GREEN, CON_YELLOW,
    CON_BLUE, CON_MAGENTA, CON_CYAN, CON_LIGHTGRAY,
    CON_GRAY, CON_BRED, CON_BGREEN, CON_BYELLOW,
    CON_BBLUE, CON_BMAGENTA, CON_BCYAN, CON_WHITE
};

/* Задаёт прямоугольник текстовой области в пикселях экрана. */
err_t con_init(u32 px, u32 py, u32 pw, u32 ph);

void con_clear(void);
void con_set_color(u8 fg_idx, u8 bg_idx);
void con_write(const char *s);
void con_printf(const char *fmt, ...);

/* Переводит строку и рисует приглашение цветом акцента (без доп. API). */
void con_prompt(const char *text);

/* Форс-флаг перерисовки: con_flush() отрисует весь буфер даже без новых
 * символов (после перекрытия окна другими поверхностями). */
void con_mark_dirty(void);

/* Есть ли неотрисованные изменения (для главного цикла: в GUI-режиме
 * dirty-консоль = запрос полного кадра композитора). */
int con_dirty(void);

/* Текущий фоновый цвет консоли 0x00RRGGBB — композитор заливает им
 * клиентскую область окна терминала перед con_flush() (швы по краям
 * сетки ячеек не должны показывать цвет подложки окна). */
u32 con_bg_rgb(void);

/* Смена якоря текстовой области БЕЗ очистки содержимого (перенос окна
 * терминала). Новая позиция обязана умещать текущую геометрию ячеек;
 * при невмещении возвращает K_ERANGE и не меняет якорь. */
err_t con_reanchor(u32 px, u32 py);

/* Смена геометрии (позиция + размер сетки) БЕЗ потери содержимого
 * (FND-2e: растягивание окна терминала). Сохраняется верхний левый
 * угол контента; новая зона очищается. */
err_t con_resize(u32 px, u32 py, u32 pw, u32 ph);

/* Отрисовка накопленных изменений в framebuffer (весь буфер, если dirty). */
void con_flush(void);

/* Точечная перекраска прямоугольника ЭКРАНА из ячеек (FND-2f): кадр TUI
 * стирает зону спрайта курсора из авторитетных данных. Не трогает
 * dirty-флаг; вне области консоли — no-op. */
void con_flush_rect_px(u32 px, u32 py, u32 pw, u32 ph);

/* Диагностика: сброс первых max_row строк буфера (позиция|символ) через
 * klog_raw — используется командой `condump` в kshell. */
void con_debug_dump(u32 max_row);

u32 con_cols(void);
u32 con_rows(void);

#endif /* FOBOS_GFX_CONSOLE_H */
