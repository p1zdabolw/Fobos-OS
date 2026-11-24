/*
 * Fobos OS — мини-оболочка экрана FND-2a: фон, панель консоли, статус-бар,
 * курсор мыши. Тоже самое, что позже рисует композитор (GUI-1), только
 * в ядре и без surface'ов — в FND-2a это честный прототип, а не архитектура.
 */
#ifndef FOBOS_GFX_UI_H
#define FOBOS_GFX_UI_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "console.h"

struct mouse_event;

/* Рисует весь экран: фон, панель с консолью, статус-бар, курсор.
 * Консоль привязывается con_resize (контент сохраняется между
 * переключениями режимов). */
err_t ui_init(void);

/* Новая сессия: сброс консоли в чистое состояние TUI-геометрии.
 * Вызывается из dsk_init (каждый вход/выход = новая консоль). */
void ui_console_session_reset(void);

/* Полная перерисовка (после fbtest). */
void ui_redraw_all(void);

/* Обработка события мыши: перемещение курсора, обновление статус-бара. */
void ui_mouse_event(const struct mouse_event *ev);

/* Позиция курсора мыши (пиксели экрана). */
void ui_mouse_pos(u32 *out_x, u32 *out_y);

/* Обновляет строку состояния (вызывается при смене секунды/движении мыши). */
void ui_statusbar_update(void);

/* 1, если наступила новая секунда и статус-бар пора обновить. */
int ui_statusbar_due(void);

#endif /* FOBOS_GFX_UI_H */
