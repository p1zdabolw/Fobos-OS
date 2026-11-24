/*
 * Fobos OS — минималистичный форматтер (по мотивам ISO snprintf, без libc).
 *
 * Поддержка: флаги '0' и '-', ширина, длины hh/h/l/ll/z, спецификаторы
 * d i u x X o p s c %.
 * Отличия от glibc (задокументированное решение, покрыто тестами):
 *  - %p печатается как "0x<hex16>" (NULL -> "(nil)");
 *  - %s с NULL -> "(null)";
 *  - неизвестный спецификатор копируется как "%<c>" без сбоя.
 *
 * Семантика возврата — как в ISO: число символов БЕЗ учёта ограничения cap
 * (усечение при buf != NULL и cap > 0), всегда NUL-терминация при cap > 0.
 * Плавающая точка не поддерживается: в ядре FND-1 нет FP-контекста.
 */
#ifndef FOBOS_LIB_VSNPRINTF_H
#define FOBOS_LIB_VSNPRINTF_H

#include <fobos/types.h>
#include <stddef.h>
#include <stdarg.h>

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t cap, const char *fmt, ...);

#endif /* FOBOS_LIB_VSNPRINTF_H */
