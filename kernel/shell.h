/*
 * Fobos OS — ядерная оболочка (kshell).
 *
 * Это НЕ будущий user-space shell — это консольный интерфейс ядра:
 * в режиме kernel console — обслуживание, на рабочем столе — «терминал»
 * сеанса пользователя. На FND-3 уедет в user-space; команды отсюда
 * мигрируют в утилиты.
 *
 * Все команды реальны: ни одна не является заглушкой.
 */
#ifndef FOBOS_SHELL_H
#define FOBOS_SHELL_H

#include <fobos/types.h>
#include "drivers/kbd.h"

/* Инициализация (баннер + приглашение). Вызывается после ui_init(). */
void shell_init(void);

/* Деинициализация (при logout: сброс строки). */
void shell_deinit(void);

/* Подать событие клавиатуры в оболочку (из главного цикла). */
void shell_input(const struct kbd_event *ev);

/* Запрошен ли logout (команда logout в сессионном режиме). */
int shell_logout_requested(void);

/* Запрос logout из GUI (Start -> питание -> Log out). */
void shell_request_logout(void);

/* Сбросить флаг logout (обработан главным циклом). */
void shell_clear_logout(void);

/* Сбросить незавершённую строку и заново вывести приглашение
 * (при перепривязке консоли: смена tui/gui, перенос окна cmd). */
void shell_reset_line(void);

#endif /* FOBOS_SHELL_H */
