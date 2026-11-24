/*
 * Fobos OS — общие экранные примитивы фазы Setup в загрузчике.
 *
 * Один источник истины для OOBE-мастера (oobe.c) и установщика (setup.c):
 * шапка с номером шага, подсказки, перенос строк, текстовое поле с
 * редактированием. Графика — lgfx поверх GOP, ввод — ConIn (lin).
 */
#ifndef FOBOS_UEFI_UISUP_H
#define FOBOS_UEFI_UISUP_H

#include "efi.h"

/* Привязка к таблицам EFI (stall в цикле опроса клавиш). */
void uisup_init(struct EFI_SYSTEM_TABLE *st);

/* Вычисляет координаты центрального бокса заданного размера (кэшируются). */
void uisup_layout(u32 box_w, u32 box_h);

u32 uisup_box_x(void);
u32 uisup_box_y(void);
u32 uisup_box_w(void);
u32 uisup_box_h(void);

/* Шапка " FOBOS OS SETUP   step N/T name" + подсказки внизу. */
void uisup_header(u32 step_no, u32 step_total, const char *step_name);
void uisup_footer(const char *hints);

/* Перенос строки по словам на две строки в пределах max_chars. */
void uisup_wrap2(const char *src, u32 max_chars,
                 char *l1, u32 l1_cap, char *l2, u32 l2_cap);

/* Типовой экран: фон, шапка, бокс, заголовок, подзаголовок, подсказки. */
void uisup_screen(u32 step_no, u32 step_total, const char *step_name,
                  const char *title, const char *subtitle,
                  const char *hints);

/* Текстовое поле с редактированием (backspace, печать, маска '*').
 * Коды: 0 = Enter, -1 = Esc. Блокирует до нажатия. */
int uisup_edit(char *buf, u32 cap, int masked);

/* Строка ошибки внутри бокса. */
void uisup_error(const char *msg);

#endif /* FOBOS_UEFI_UISUP_H */
