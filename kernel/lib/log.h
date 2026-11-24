/*
 * Fobos OS — единый logging-подсистема ядра.
 *
 * Правила проекта: НИКАКИХ printf в ядре — только klog. Каждая строка идёт
 * в serial (COM1) и в кольцевой буфер в памяти (для crash-dump в FND-2+).
 *
 * Формат строки:  [<tsc>] <LEVEL> [<subsys>] <message>\n
 * Метка времени — сырые тики TSC (некалиброванные на FND-1; калибровка через
 * ACPI PM-timer входит в FND-2 — см. docs/roadmap.md).
 *
 * Потокобезопасность: внутренний spinlock; вызовы IRQ-save вариантов не требуются.
 */
#ifndef FOBOS_LIB_LOG_H
#define FOBOS_LIB_LOG_H

#include <fobos/types.h>

enum klog_level {
    KLOG_TRACE = 0,
    KLOG_DEBUG = 1,
    KLOG_INFO  = 2,
    KLOG_WARN  = 3,
    KLOG_ERROR = 4,
    KLOG_PANIC = 5
};

/* Инициализация. Идемпотентна; до первого вызова вывод подавляется. */
void klog_init(void);

void klog_set_level(enum klog_level lvl);
enum klog_level klog_get_level(void);

void klog(enum klog_level lvl, const char *subsys, const char *fmt, ...);

/* Вывод без префикса (для многострочных дампов, где префикс уже напечатан). */
void klog_raw(const char *s);

/* Дополнительный синк (например, fbcon): вызывается для каждой полной строки
 * с завершающим '\n' после serial. Только main-контекст. */
void klog_set_sink(void (*fn)(const char *line));

/* Минимальный уровень строк, попадающих в синк (по умолчанию TRACE — всё).
 * Терминал пользователя — не журнал: kmain ставит WARN, чтобы INFO-поток
 * подсистем не засорял экран; serial получает всё как раньше. */
void klog_set_sink_level(enum klog_level lvl);

/* Печатает сообщение уровня PANIC и остановляет систему. Не возвращает управление. */
void klog_panic(const char *subsys, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

/* Число строк, удерживаемых кольцевым буфером (для будущего crash-dump). */
u32 klog_ring_lines(void);

#endif /* FOBOS_LIB_LOG_H */
