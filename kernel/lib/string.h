/*
 * Fobos OS — строковые/памятные примитивы ядра (freestanding).
 * Обязательны к определению: gcc в freestanding-режиме вправе эмитить вызовы
 * memcpy/memset/memmove/memcmp для структурных копий.
 */
#ifndef FOBOS_LIB_STRING_H
#define FOBOS_LIB_STRING_H

#include <fobos/types.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strncmp(const char *a, const char *b, size_t n);
int   strcmp(const char *a, const char *b);

#endif /* FOBOS_LIB_STRING_H */
